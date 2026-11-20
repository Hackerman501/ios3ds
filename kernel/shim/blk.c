/*
 * ios3ds shim: block I/O for XNU's memory-disk driver (bsd/dev/memdev.c).
 * The patched mdevstrategy() lands here; requests go to the ARM9 through the
 * block mailbox and a bounce buffer; the ARM9 reads/writes sd:/ios3ds/rootfs.hfs.
 * Runs in the kernel, MMU + D-cache on: explicit cache maintenance on every
 * line shared with the (uncached) ARM9.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"

typedef void *buf_t;
/* xnu-1504 KPI (Thumb functions in the 4.2.1 kernel; |1 for interworking) */
#define KPI(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define buf_count     KPI(0x800781c0, u32, buf_t)
#define buf_setresid  KPI(0x800781d4, void, buf_t, u32)
#define buf_blkno     KPI(0x800782a4, long long, buf_t)
#define buf_seterror  KPI(0x80078158, void, buf_t, int)
#define buf_biodone   KPI(0x800793f4, void, buf_t)
#define buf_map       KPI(0x800789a0, int, buf_t, void **)
#define buf_unmap     KPI(0x800789f2, int, buf_t)
#define buf_flags     KPI(0x80078198, int, buf_t)
/* locking + sleeping (kernelcache symbol table; Thumb unless noted) */
#define lck_grp_attr_alloc_init KPI(0x8001ae22, void *, void)
#define lck_grp_alloc_init      KPI(0x8001ae00, void *, const char *, void *)
#define lck_attr_alloc_init     KPI(0x8001ade8, void *, void)
#define lck_mtx_alloc_init      KPI(0x80062008, void *, void *, void *)
#define lck_mtx_lock   ((void (*)(void *))0x80063000)     /* ARM */
#define lck_mtx_unlock ((void (*)(void *))0x800631a0)     /* ARM */
#define assert_wait_timeout     KPI(0x8001f02a, int, void *, int, u32, u32)
#define thread_block            KPI(0x8001edcc, int, void *)
#define THREAD_UNINT   0
#define NSEC_PER_USEC  1000u
#define B_READ 0x00000001
#define EIO 5

#define MBOX   ((volatile blkmbox_t *)(SHIM_VA + SHIM_BLK_OFF))
#define BOUNCE ((u8 *)(SHIM_VA + SHIM_BOUNCE_OFF))

static inline void dclean(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

void shim_l2_inv_kva(const volatile void *va, u32 len);    /* entry.S: New 3DS L2 (docs/n3ds.md) */
static inline void dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    shim_l2_inv_kva(p, len);
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static void copy(u8 *d, const u8 *s, u32 n)
{
    if ((((u32)d | (u32)s | n) & 3) == 0) {
        for (u32 i = 0; i < n / 4; i++) ((u32 *)d)[i] = ((const u32 *)s)[i];
    } else {
        while (n--) *d++ = *s++;
    }
}

static inline u32 irq_save(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static inline void irq_restore(u32 cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

/* md0 requests are serialized by a kernel mutex, and the wait for the ARM9
   runs with interrupts on and sleeps: before, the whole request ran with
   IRQs off, so every disk read (~20 ms on hardware, thousands per app
   launch) froze timers, scheduling and VBlank for the whole kernel. A
   SpringBoard Home press could then be seen as a hold (its hold timer
   expired while it was stalled between the down and the up; soak test,
   re-log 30). If the lock can't be created, the old IRQs-off path is used. */
static void *blk_lock;
static int blk_lock_tried;

static void blk_lock_init(void)
{
    u32 f = irq_save();
    int first = !blk_lock_tried;
    blk_lock_tried = 1;
    irq_restore(f);
    if (!first) return;
    void *ga = lck_grp_attr_alloc_init();
    void *grp = ga ? lck_grp_alloc_init("ios3ds-md0", ga) : 0;
    void *la = lck_attr_alloc_init();
    blk_lock = grp && la ? lck_mtx_alloc_init(grp, la) : 0;
}

/* One transfer of <= SHIM_BOUNCE_SIZE through the ARM9; returns status.
   sleeping = 1: interrupts are on and the caller holds blk_lock, so wait by
   spinning briefly, then sleeping in 250 us steps (other threads run). */
static int xfer(u32 op, u32 lba, u32 count, int sleeping, u32 xbytes, u32 xflags)
{
    volatile blkmbox_t *m = MBOX;
    u32 seq = m->seq + 1;
    m->op = op;
    m->lba = lba;
    m->count = count;
    m->xnu_bytes = xbytes;      /* for the ARM9's md0 profile only */
    m->xnu_flags = xflags;
    m->seq = seq;
    dclean(&m->seq, 32);
    for (u32 spins = 0;; spins++) {
        dinval(&m->done_seq, 32);
        if (m->done_seq == seq) break;
        if (sleeping && spins >= 200) {
            assert_wait_timeout((void *)&m->done_seq, THREAD_UNINT, 250, NSEC_PER_USEC);
            thread_block(0);
        }
    }
    return m->status;
}

void shim_input_register(void);
void shim_gpu_register(void);
void shim_net_register(void);
void shim_mcu_register(void);

void shim_mdevstrategy(buf_t bp)
{
    /* first block I/O happens in a normal, blockable thread: register the
       kern.ios3ds_input sysctl here (idempotent) */
    shim_input_register();
    shim_gpu_register();            /* ios3ds_gpu (pica.c, if gpu.bin is loaded); the GPU itself is brought up on first use */
    shim_net_register();        /* kern.ios3ds_net (tools/ios3ds-wifi) */
    shim_mcu_register();        /* kern.ios3ds_mcu{,ctl} (tools/ios3ds-mcu, docs/mcu.md) */
    volatile blkmbox_t *m = MBOX;
    u32 total = buf_count(bp);
    long long blk = buf_blkno(bp);
    u32 bflags = (u32)buf_flags(bp);
    int reading = bflags & B_READ;
    u8 *data = 0;
    int err = 0;

    blk_lock_init();
    dinval(m, 32);
    int mapped = m->magic == BLK_MAGIC && buf_map(bp, (void **)&data) == 0 && data;
    if (mapped) {
        /* fault the whole buffer in first: interrupts on (the UPL mapping is
           populated lazily; vm_fault can't run with IRQs off) and before
           taking blk_lock (a fault must never wait for md0 behind us) */
        for (u32 off = 0; off < total; off = (off | 0xFFF) + 1) {
            volatile u8 *q = data + off;
            u8 v = *q;
            if (reading) *q = v;
        }
    }
    if (blk_lock) lck_mtx_lock(blk_lock);
    if (!mapped) {
        err = EIO;
    } else {
        u32 lba = (u32)blk, done = 0;
        while (done < total) {
            u32 n = total - done;
            if (n > SHIM_BOUNCE_SIZE) n = SHIM_BOUNCE_SIZE;
            u32 secs = (n + 511) / 512;
            int sleeping = blk_lock != 0;
            u32 xflags = bflags | (done ? 0 : 0x80000000u);
            u32 flags = sleeping ? 0 : irq_save();
            int st;
            if (reading) {
                dinval(BOUNCE, secs * 512);
                st = xfer(BLK_READ, lba, secs, sleeping, total, xflags);
                if (!st) {
                    dinval(BOUNCE, secs * 512);
                    copy(data + done, BOUNCE, n);
                }
                m->nread++;
            } else {
                copy(BOUNCE, data + done, n);
                dclean(BOUNCE, secs * 512);
                st = xfer(BLK_WRITE, lba, secs, sleeping, total, xflags);
                m->nwrite++;
            }
            if (!sleeping) irq_restore(flags);
            if (st) { err = EIO; break; }
            done += n;
            lba += secs;
        }
        buf_unmap(bp);
    }
    m->nreq++;
    if (err) {
        m->nerr++;
        buf_seterror(bp, err);
        buf_setresid(bp, total);
    } else {
        buf_setresid(bp, 0);
    }
    dclean(&m->nreq, 16);
    if (blk_lock) lck_mtx_unlock(blk_lock);
    buf_biodone(bp);
}
