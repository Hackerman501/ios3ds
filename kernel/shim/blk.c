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

static inline void dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
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

/* One transfer of <= SHIM_BOUNCE_SIZE through the ARM9; returns status.
   The caller holds interrupts off for the whole request + bounce copy: kernel
   threads are preemptible and the mailbox/bounce buffer are single-slot. */
static int xfer(u32 op, u32 lba, u32 count)
{
    volatile blkmbox_t *m = MBOX;
    u32 seq = m->seq + 1;
    m->op = op;
    m->lba = lba;
    m->count = count;
    m->seq = seq;
    dclean(&m->seq, 32);
    for (;;) {
        dinval(&m->done_seq, 32);
        if (m->done_seq == seq) break;
    }
    return m->status;
}

void shim_input_register(void);

void shim_mdevstrategy(buf_t bp)
{
    /* first block I/O happens in a normal, blockable thread: register the
       kern.ios3ds_input sysctl here (idempotent) */
    shim_input_register();
    volatile blkmbox_t *m = MBOX;
    u32 total = buf_count(bp);
    long long blk = buf_blkno(bp);
    int reading = buf_flags(bp) & B_READ;
    u8 *data = 0;
    int err = 0;

    dinval(m, 32);
    if (m->magic != BLK_MAGIC || buf_map(bp, (void **)&data) != 0 || !data) {
        err = EIO;
    } else {
        u32 lba = (u32)blk, done = 0;
        while (done < total) {
            u32 n = total - done;
            if (n > SHIM_BOUNCE_SIZE) n = SHIM_BOUNCE_SIZE;
            u32 secs = (n + 511) / 512;
            /* fault the buffer pages in while interrupts are still on (the UPL
               mapping is populated lazily; vm_fault can't run with IRQs off) */
            for (u32 off = 0; off < n; off = (off | 0xFFF) + 1) {
                volatile u8 *q = data + done + off;
                u8 v = *q;
                if (reading) *q = v;
            }
            u32 flags = irq_save();
            int st;
            if (reading) {
                dinval(BOUNCE, secs * 512);
                st = xfer(BLK_READ, lba, secs);
                if (!st) {
                    dinval(BOUNCE, secs * 512);
                    copy(data + done, BOUNCE, n);
                }
                m->nread++;
            } else {
                copy(BOUNCE, data + done, n);
                dclean(BOUNCE, secs * 512);
                st = xfer(BLK_WRITE, lba, secs);
                m->nwrite++;
            }
            irq_restore(flags);
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
    buf_biodone(bp);
}
