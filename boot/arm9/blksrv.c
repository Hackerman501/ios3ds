/*
 * Block server: serves XNU's md0 (memdev -> kernel shim -> mailbox) from a
 * file on the SD card. Polled from the monitor loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "blksrv.h"
#include "log.h"
#include "util.h"
#include "fatfs/ff.h"

static FIL f;
static DWORD clmt[32768];   /* 128 KiB: a 1 GiB image copied onto a busy card can have ~15k fragments */
static bool open_ok;
static u32 served, errors, sectors_moved;

/* HFS+ volume header at byte 1024: 'H+' or 'HX', version, then the 32-bit
   big-endian attributes. Bit 8 (kHFSVolumeUnmountedMask, 0x0100) is set by a
   clean unmount and cleared at mount. Our image is not journaled and Apple's
   launchctl fsck cannot open /dev/md0 while it is mounted (EBUSY, re-log 86),
   so this bit is the only durable record of the last shutdown. Read it when
   the image is opened (before XNU mounts md0) and set it again when iOS asks
   us to halt: PEHaltRestart runs after the file systems were unmounted. */
#define HFS_VH_OFF      1024u
#define HFS_VH_ATTR     4u
#define HFS_UNMOUNTED   0x0100u
static bool vh_ok;
static u32 vh_attrs;

static void vh_read(void)
{
    u8 b[8];
    UINT n = 0;
    FRESULT r = f_lseek(&f, HFS_VH_OFF);
    if (r == FR_OK) r = f_read(&f, b, sizeof b, &n);
    vh_ok = r == FR_OK && n == sizeof b && b[0] == 'H' && (b[1] == '+' || b[1] == 'X');
    if (vh_ok) vh_attrs = (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7];
}

bool blksrv_mark_clean(void)
{
    if (!open_ok) return false;
    /* Re-read the volume header from disk: the cached copy taken at boot is
       stale by now (XNU clears bit 8 on mount and may set it again on a
       clean unmount), so a cached "already clean" answer would either skip a
       needed write or, worse, trust a dirty image. Only a live clean halt
       ever calls this, so setting the bit here is always correct. */
    u8 hdr[8];
    UINT n = 0;
    FRESULT r = f_lseek(&f, HFS_VH_OFF);
    if (r == FR_OK) r = f_read(&f, hdr, sizeof hdr, &n);
    vh_ok = r == FR_OK && n == sizeof hdr && hdr[0] == 'H' && (hdr[1] == '+' || hdr[1] == 'X');
    if (!vh_ok) {
        log_warn("rootfs: no HFS+ volume header at byte %u: cannot set the clean flag\n", HFS_VH_OFF);
        return false;
    }
    vh_attrs = (u32)hdr[4] << 24 | (u32)hdr[5] << 16 | (u32)hdr[6] << 8 | hdr[7];
    if (vh_attrs & HFS_UNMOUNTED) {
        log_ok("[blk] rootfs.hfs: already cleanly unmounted (attributes 0x%08x)\n", vh_attrs);
        return true;
    }
    u32 clean = vh_attrs | HFS_UNMOUNTED;
    u8 b[4] = { (u8)(clean >> 24), (u8)(clean >> 16), (u8)(clean >> 8), (u8)clean };
    r = f_lseek(&f, HFS_VH_OFF + HFS_VH_ATTR);
    n = 0;
    if (r == FR_OK) r = f_write(&f, b, sizeof b, &n);
    if (r == FR_OK && n != sizeof b) r = FR_DENIED;
    if (r == FR_OK) r = f_sync(&f);
    if (r == FR_OK) {
        vh_attrs = clean;
        log_ok("[blk] rootfs.hfs: marked cleanly unmounted after iOS shutdown (attributes 0x%08x)\n", vh_attrs);
    } else {
        log_err("[blk] rootfs.hfs: could not set the clean flag (FRESULT %d)\n", (int)r);
    }
    return r == FR_OK;
}

/* md0 profile (boot.cfg ioprof=1/2, docs/boot-perf.md): 1 = one [io] line per
   second with md0 activity, 2 = also one [blk] line per request */
static u32 prof_level, prof_hz;
static u32 (*prof_now)(void);
static struct io_acc {
    u32 chunks, bufs, rd_sec, wr_sec, meta_sec, page_sec, seq, svc;
} acc, tot;
static u32 prof_t0, last_end = ~0u;

void blksrv_profile(u32 level, u32 (*now)(void), u32 hz)
{
    prof_level = level;
    prof_now = now;
    prof_hz = hz;
    if (level) prof_t0 = now();
}

static u32 ms(u32 ticks) { return (u32)((u64)ticks * 1000 / prof_hz); }

/* call from the monitor loop: the per-second summary */
void blksrv_prof_tick(void)
{
    if (!prof_level) return;
    u32 t = prof_now();
    if (t - prof_t0 < prof_hz) return;
    if (acc.chunks) {
        u32 tm = ms(t);
        logp("[io] t=%u.%03u %u bufs %u chunks | rd %u KiB wr %u KiB (meta %u, pagein %u) seq %u%% | "
             "arm9 %u ms | total %u bufs %u KiB %u ms\n",
             tm / 1000, tm % 1000, acc.bufs, acc.chunks, acc.rd_sec / 2, acc.wr_sec / 2, acc.meta_sec / 2,
             acc.page_sec / 2, acc.seq * 100 / acc.chunks, ms(acc.svc), tot.bufs, (tot.rd_sec + tot.wr_sec) / 2,
             ms(tot.svc));
    }
    memset(&acc, 0, sizeof(acc));
    prof_t0 = t;
}

static void prof_one(u32 op, u32 lba, u32 cnt, u32 xbytes, u32 xflags, u32 t0, u32 t1)
{
    struct io_acc *a[2] = { &acc, &tot };
    for (int i = 0; i < 2; i++) {
        a[i]->chunks++;
        if (xflags & 0x80000000u) a[i]->bufs++;
        if (op == BLK_READ) a[i]->rd_sec += cnt;
        else a[i]->wr_sec += cnt;
        if (xflags & 0x100) a[i]->meta_sec += cnt;
        if (xflags & 0x80) a[i]->page_sec += cnt;
        if (lba == last_end) a[i]->seq++;
        a[i]->svc += t1 - t0;
    }
    last_end = lba + cnt;
    if (prof_level >= 2) {
        u32 tm = ms(t0);
        logp("[blk] %u.%03u %c %u+%u buf %u f%x %u us\n", tm / 1000, tm % 1000, op == BLK_READ ? 'r' : 'w',
             lba, cnt, xbytes, xflags, (u32)((u64)(t1 - t0) * 1000000 / prof_hz));
    }
}

#define MBOX ((volatile blkmbox_t *)(SHIM_PA + SHIM_BLK_OFF))
#define BOUNCE ((u8 *)(SHIM_PA + SHIM_BOUNCE_OFF))

int blksrv_open(const char *path, u32 *sectors)
{
    volatile blkmbox_t *m = MBOX;
    memset((void *)m, 0, sizeof(*m));
    FRESULT r = f_open(&f, path, FA_READ | FA_WRITE);
    if (r != FR_OK) return -(int)r;
    vh_ok = false;
    vh_attrs = 0;
    vh_read();
    if (!vh_ok)
        log_warn("rootfs %s: no HFS+ volume header at byte %u: cannot check the clean-unmount flag\n",
                 path, HFS_VH_OFF);
    else if (!(vh_attrs & HFS_UNMOUNTED))
        log_warn("[blk] rootfs.hfs: NOT cleanly unmounted (attributes 0x%08x, bit 8 clear): the last iOS run "
                 "was cut short. Apple's fsck cannot check md0 while it is mounted (EBUSY), so the volume "
                 "was mounted as-is; back up the image if files look wrong\n", vh_attrs);
    else
        log_ok("[blk] rootfs.hfs: cleanly unmounted (attributes 0x%08x)\n", vh_attrs);
    f.cltbl = clmt;
    clmt[0] = ARRAY_SIZE(clmt);
    r = f_lseek(&f, CREATE_LINKMAP);
    if (r != FR_OK) {
        log_warn("blksrv: fast-seek map needs %u entries (have %u): seeks will be slow\n",
                 (unsigned)clmt[0], (unsigned)ARRAY_SIZE(clmt));
        f.cltbl = NULL;
    }
    *sectors = f_size(&f) / 512;
    m->dev_sectors = *sectors;
    m->magic = BLK_MAGIC;
    open_ok = true;
    return 0;
}

/* returns true if a request was served */
bool blksrv_poll(void)
{
    volatile blkmbox_t *m = MBOX;
    if (!open_ok || m->seq == m->done_seq) return false;
    u32 seq = m->seq, op = m->op, lba = m->lba, cnt = m->count;
    u32 xbytes = m->xnu_bytes, xflags = m->xnu_flags;
    u32 t0 = prof_level ? prof_now() : 0;
    int st = 0;
    UINT bx = 0;
    if (cnt == 0 || cnt * 512 > SHIM_BOUNCE_SIZE || (u64)lba + cnt > m->dev_sectors) {
        st = 1000;
    } else {
        FRESULT r = f_lseek(&f, (FSIZE_t)lba * 512);
        if (r == FR_OK) {
            r = op == BLK_READ ? f_read(&f, BOUNCE, cnt * 512, &bx)
                               : f_write(&f, BOUNCE, cnt * 512, &bx);
            if (r == FR_OK && op == BLK_WRITE) r = f_sync(&f);
        }
        st = r != FR_OK ? (int)r : (bx != cnt * 512 ? 1001 : 0);
    }
    if (st) {
        errors++;
        log_err("[blk] %s lba %u x%u failed (%d)\n", op == BLK_READ ? "read" : "write", lba, cnt, st);
    }
    served++;
    if (!st) sectors_moved += cnt;
    u32 t1 = prof_level ? prof_now() : 0;
    m->status = st;
    m->done_seq = seq;
    if (prof_level && !st) prof_one(op, lba, cnt, xbytes, xflags, t0, t1);  /* after: the ARM11 goes on */
    return true;
}

u32 blksrv_sectors(void) { return sectors_moved; }

void blksrv_stats(u32 *nserved, u32 *nerr)
{
    *nserved = served;
    *nerr = errors;
}
