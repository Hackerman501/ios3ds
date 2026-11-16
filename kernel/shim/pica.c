/*
 * ios3ds shim: the PICA200 GPU service, top-level sysctl node "ios3ds_gpu"
 * (not under "kern.": see pica_proto.h).
 *
 * Userland (tools/ios3ds-gl's PICA back end) never touches GPU registers or
 * physical memory. It sends requests (tools/ios3ds-pica/include/pica_proto.h)
 * as one extra MIB component = the address of a pica_req_t; this file copies
 * the request in, runs it through the driver core (tools/ios3ds-pica/src/
 * pica_drv.c, the same code the host tests run against the software model)
 * and copies the result back. One request runs at a time.
 *
 * First use ("bring-up"), in the caller's thread:
 *   - PDN_GPU_CNT (0x10141200): Luma leaves the GPU clocked and out of reset
 *     (0x1007F) because it initialised the screens; if it is not, the blocks
 *     are taken out of reset here and that is logged
 *   - CFG11_GPUPROT (0x10140140) = 0: the GPU may read and write all of FCRAM
 *   - the arena: 8 MiB (or less) of physically contiguous kernel memory from
 *     IOMallocContiguous; the GPU addresses it by physical address
 *   - the GX block itself (already mapped at PDC_VA by entry.S) is set up by
 *     pica_drv_init(); the display controllers in it are not touched
 *
 * Kernel facts (iPod2,1 4.2.1 8C148, xnu-1504.58.28, kernelcache symbols;
 * ARM/Thumb from the symbol table's N_ARM_THUMB_DEF bit):
 *   copyin 0x80067c84 (ARM)  copyout 0x80067d04 (ARM)   (uaddr is 32 bit)
 *   IOMallocContiguous 0x801a70e0  ml_io_map 0x80067780  IODelay 0x801a6c2c
 *   IOSleep 0x801a6c40  printf 0x8001c4c4  proc_selfpid 0x8014f1be
 *   proc_find 0x8014f94c  proc_rele 0x8014f862            (all Thumb)
 *   sysctl_root passes a CTLTYPE_NODE handler (name + consumed, namelen -
 *   consumed), which is how the request pointer gets here.
 * Everything is logged with a "[pica]" prefix through the kernel's printf,
 * i.e. the console ring the ARM9 shows and saves.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "pica_drv.h"
#include "pica_regs.h"

struct sysctl_oid {
    void       *oid_parent;
    void       *oid_link;
    int         oid_number;
    int         oid_kind;
    void       *oid_arg1;
    int         oid_arg2;
    const char *oid_name;
    int       (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};

#define CTLFLAG_RD      0x80000000
#define CTLTYPE_NODE    1
#define OID_AUTO        (-1)
#define EFAULT 14
#define EINVAL 22

#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))    /* Thumb */
#define KPA(addr, ret, ...) ((ret (*)(__VA_ARGS__))(addr))          /* ARM */
#define sysctl_register_oid   KPT(0x80159c54, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPT(0x80159242, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_ROOT_CHILDREN  ((void *)0x8027f7a8)      /* _sysctl__children: top level */
#define k_copyin              KPA(0x80067c84, int, u32, void *, u32)
#define k_copyout             KPA(0x80067d04, int, const void *, u32, u32)
#define IOMallocContiguous    KPT(0x801a70e0, void *, u32, u32, uint32_t *)
#define ml_io_map             KPT(0x80067780, void *, u32, u32)
#define IODelay               KPT(0x801a6c2c, void, u32)
#define IOSleep               KPT(0x801a6c40, void, u32)
#define k_printf              KPT(0x8001c4c4, void, const char *, ...)
#define proc_selfpid          KPT(0x8014f1be, int, void)
#define proc_find             KPT(0x8014f94c, void *, int)
#define proc_rele             KPT(0x8014f862, int, void *)

#define GX_VA       0xBFE00000u         /* entry.S: 1 MiB device section over PA 0x10400000 */
#define CFG11_PA    0x10140000u
#define PDN_PA      0x10141000u
#define GPUPROT_OFF 0x140u
#define GPU_CNT_OFF 0x200u
#define GPU_CNT_ON  (PDN_GPU_CNT_CLK_EN | PDN_GPU_CNT_NORST_ALL)

static pica_drv_t drv;
static uint32_t bringup_state;               /* 0 = not tried, 1 = done (drv.state says how it went) */
static volatile uint32_t busy;

static uint32_t p_rd(uint32_t off) { return *(vu32 *)(GX_VA + off); }
static void p_wr(uint32_t off, uint32_t v) { *(vu32 *)(GX_VA + off) = v; }
static int p_copyin(pica_uptr_t src, void *dst, uint32_t len) { return k_copyin((uint32_t)src, dst, len); }
static int p_copyout(const void *src, pica_uptr_t dst, uint32_t len) { return k_copyout(src, (uint32_t)dst, len); }

/* The arena is ordinary cached kernel memory and the GPU reads and writes
   DRAM directly: write dirty lines back before it reads, drop our copies
   after it wrote. Every block is a multiple of 128 bytes, so no cache line
   is shared with someone else's data. */
static void p_to_gpu(void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static void p_from_gpu(void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static void p_udelay(uint32_t us) { IODelay(us); }
static int p_alive(uint32_t pid)
{
    void *p = proc_find((int)pid);
    if (!p)
        return 0;
    proc_rele(p);
    return 1;
}

static pica_plat_t plat;                /* filled at bring-up (k_printf is not a constant expression) */

static void bringup(void)
{
    static const uint32_t sizes[] = { 0x800000, 0x400000, 0x200000 };
    vu32 *pdn, *cfg11;
    uint32_t cnt = 0, prot = 0, pa = 0, size = 0, i, st;
    void *va = 0;

    bringup_state = 1;
    plat.rd = p_rd; plat.wr = p_wr; plat.copyin = p_copyin; plat.copyout = p_copyout;
    plat.to_gpu = p_to_gpu; plat.from_gpu = p_from_gpu; plat.udelay = p_udelay; plat.alive = p_alive;
    plat.log = k_printf;
    drv.p = &plat;
    drv.state = PICA_ERR_GPU_OFF;

    pdn = (vu32 *)ml_io_map(PDN_PA, 0x1000);
    cfg11 = (vu32 *)ml_io_map(CFG11_PA, 0x1000);
    if (!pdn || !cfg11) {
        k_printf("[pica] cannot map PDN/CFG11 (%p %p): no GPU\n", pdn, cfg11);
        return;
    }
    cnt = pdn[GPU_CNT_OFF / 4];
    prot = cfg11[GPUPROT_OFF / 4];
    k_printf("[pica] bring-up: PDN_GPU_CNT 0x%08x, GPUPROT 0x%08x, GX clock 0x%08x, GX stat 0x%08x\n",
             cnt, prot, p_rd(0x004), p_rd(0x034));
    drv.gpu_cnt = cnt;
    if ((cnt & GPU_CNT_ON) != GPU_CNT_ON) {
        /* Not what Luma leaves behind after screen init. Clock on, blocks out
           of reset (libn3ds PDN_controlGpu); without Luma's display setup the
           LCDs stay dark, but offscreen rendering can work. */
        k_printf("[pica] GPU was not running (expected 0x%08x): enabling clock, releasing resets\n", GPU_CNT_ON);
        pdn[GPU_CNT_OFF / 4] = PDN_GPU_CNT_CLK_EN;
        IODelay(100);
        pdn[GPU_CNT_OFF / 4] = GPU_CNT_ON;
        IODelay(100);
        p_wr(0x004, 0x70100);           /* GX clock setup, as libn3ds/Luma write it after a reset */
        if ((pdn[GPU_CNT_OFF / 4] & GPU_CNT_ON) != GPU_CNT_ON) {
            k_printf("[pica] PDN_GPU_CNT stays 0x%08x: GPU unusable\n", pdn[GPU_CNT_OFF / 4]);
            return;
        }
    }
    cfg11[GPUPROT_OFF / 4] = 0;         /* GPU DMA may reach all of FCRAM (the arena lives there) */

    drv.state = PICA_ERR_NO_ARENA;
    for (i = 0; i < sizeof sizes / sizeof sizes[0] && !va; i++) {
        size = sizes[i];
        va = IOMallocContiguous(size, 0x1000, &pa);
    }
    if (!va || !pa) {
        k_printf("[pica] no physically contiguous memory for the GPU arena (tried down to %u KiB)\n", size >> 10);
        return;
    }
    st = pica_drv_init(&drv, &plat, va, pa, size, cnt);
    k_printf("[pica] arena %u KiB at VA %p = PA 0x%08x; driver %s (state %u)\n",
             size >> 10, va, pa, st == PICA_OK ? "ready" : "FAILED", st);
}

static void run(pica_req_t *r, uint32_t pid)
{
    uint32_t *a = r->a;
    if (!bringup_state)
        bringup();
    switch (r->op) {
    case PICA_OP_INFO:
        pica_drv_info(&drv, (pica_info_t *)a);
        r->status = PICA_OK;
        break;
    case PICA_OP_ALLOC:    r->status = pica_drv_alloc(&drv, pid, a[0], a[1], &a[0]); break;
    case PICA_OP_FREE:     r->status = pica_drv_free(&drv, pid, a[0]); break;
    case PICA_OP_FREE_ALL: pica_drv_free_all(&drv, pid); r->status = PICA_OK; break;
    case PICA_OP_WRITE:    r->status = pica_drv_write(&drv, a[0], a[1], a[2]); break;
    case PICA_OP_READ:     r->status = pica_drv_read(&drv, a[0], a[1], a[2]); break;
    case PICA_OP_SUBMIT:   r->status = pica_drv_submit(&drv, a[0], a[1], a[2], a[3], a[4]); break;
    case PICA_OP_FILL:     r->status = pica_drv_fill(&drv, a); break;
    case PICA_OP_TRANSFER: r->status = pica_drv_transfer(&drv, a[0], a[1], a[2], a[3], a[4]); break;
    default:               r->status = PICA_ERR_BAD_OP; break;
    }
}

static int take(void)
{
    uint32_t cpsr, got;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr | 0x80) : "memory");
    got = !busy;
    if (got)
        busy = 1;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    return (int)got;
}

static int pica_sysctl(struct sysctl_oid *oidp, void *arg1, int namelen, void *req)
{
    int *name = (int *)arg1;
    pica_req_t r;
    if (namelen == 0) {                 /* "sysctl ios3ds_gpu": status only, no side effects */
        pica_info_t info;
        if (bringup_state) {
            pica_drv_info(&drv, &info);
        } else {
            uint32_t *w = (uint32_t *)&info, i;
            for (i = 0; i < sizeof info / 4; i++)
                w[i] = 0;
            info.version = PICA_PROTO_VERSION;
            info.state = PICA_ERR_GPU_OFF;      /* not brought up yet */
        }
        return sysctl_handle_opaque(oidp, &info, sizeof info, req);
    }
    if (namelen != 1)
        return EINVAL;
    if (k_copyin((uint32_t)name[0], &r, sizeof r))
        return EFAULT;
    while (!take())
        IOSleep(1);
    run(&r, (uint32_t)proc_selfpid());
    busy = 0;
    if (k_copyout(&r, (uint32_t)name[0], sizeof r))
        return EFAULT;
    return 0;
}

static struct sysctl_oid pica_oid = {
    .oid_parent  = SYSCTL_ROOT_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_NODE),
    .oid_name    = PICA_SYSCTL_LEAF,
    .oid_handler = pica_sysctl,
    .oid_fmt     = "N",
};

/* Thread context only. Idempotent. Only registers the node: the GPU is not
   touched until the first request. */
void shim_pica_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&pica_oid);
}
