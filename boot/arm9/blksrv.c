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

#define MBOX ((volatile blkmbox_t *)(SHIM_PA + SHIM_BLK_OFF))
#define BOUNCE ((u8 *)(SHIM_PA + SHIM_BOUNCE_OFF))

int blksrv_open(const char *path, u32 *sectors)
{
    volatile blkmbox_t *m = MBOX;
    memset((void *)m, 0, sizeof(*m));
    FRESULT r = f_open(&f, path, FA_READ | FA_WRITE);
    if (r != FR_OK) return -(int)r;
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
    m->status = st;
    m->done_seq = seq;
    return true;
}

u32 blksrv_sectors(void) { return sectors_moved; }

void blksrv_stats(u32 *nserved, u32 *nerr)
{
    *nserved = served;
    *nerr = errors;
}
