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
 *
 * The GPU display mirror (boot/common/gpu_mirror.h, docs/pica200.md 9) lives
 * here too, because it shares the GPU with the GL service:
 *   - shim_gpu_thread_poll() runs in thread context (the input daemon's and
 *     the GL service's sysctl calls): once the ARM9 says the iOS display is
 *     on, it brings the GPU up, self-tests the mirror and switches it on;
 *   - shim_gpu_irq() runs from entry.S on every VBlank and timer interrupt
 *     (IRQ mode, private stack, IRQs masked): it starts a frame every
 *     vbl_div-th VBlank and advances the one in flight. It never waits for
 *     the GPU (unless boot.cfg gpu_spin_us says so).
 *   - `busy` arbitrates: 1 = a GL request owns the GPU, 2 = a mirror frame.
 *     A VBlank that finds GL busy skips its frame; a GL request that finds a
 *     frame in flight sleeps until the interrupts have finished it.
 * boot.cfg gpu=0 (gmbox_t.gpu_enable) keeps the shim away from the GPU
 * entirely: the GL engine falls back to its software rasterizer and the
 * ARM9 keeps copying the screen.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "pica_drv.h"
#include "pica_regs.h"
#include "pica_mirror.h"
#include "gpu_mirror.h"

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
#define PRIV_VA     0xBFF00000u         /* entry.S: MPCore private region (watchdog = clock) */
#define AXI_PA      0x1FF00000u         /* entry.S: AXI WRAM section (fake S5L register pages) */
#define AXI_VA      0xBFD00000u
#define GM          ((volatile gmbox_t *)(SHIM_VA + SHIM_GM_OFF))
#define CFG11_PA    0x10140000u
#define PDN_PA      0x10141000u
#define GPUPROT_OFF 0x140u
#define GPU_CNT_OFF 0x200u
#define GPU_CNT_ON  (PDN_GPU_CNT_CLK_EN | PDN_GPU_CNT_NORST_ALL)

/* first words of the GPU part (link.ld puts .gpuhdr at GPU_PART_VA): the
   main shim calls in only when this is there (gpu_glue.c) */
__attribute__((section(".gpuhdr"), used))
static const uint32_t gpu_part_hdr[4] = { GPU_PART_MAGIC, 1, GPU_PART_PA, GM_BLOB_PA };

static pica_drv_t drv;
static uint32_t bringup_state;               /* 0 = not tried, 1 = done (drv.state says how it went) */
static volatile uint32_t busy;              /* 0 free, 1 GL request, 2 mirror frame */

/* ---- the ARM9 <-> ARM11 display mailbox (gmbox_t at shim+0x7F00) ---- */
extern uint32_t cfg_clcd_pa;                /* entry.S: fake CLCD page (PA), patched in by mkboot */

static void dc_clean(volatile void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
static void dc_inval(volatile void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}
/* lines 0-1 are the ARM9's (it writes FCRAM uncached): drop our copy first */
static void gm_fetch(void) { dc_inval(GM, 64); }
/* lines 2-4 are ours: write them back so the ARM9 sees them */
static void gm_publish(void) { dc_clean(&GM->state, 96); }
static int gm_valid(void) { gm_fetch(); return GM->magic == GM_MAGIC; }

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
    if (gm_valid() && !GM->gpu_enable) {
        k_printf("[pica] GPU switched off in boot.cfg (gpu=0): not touching it\n");
        return;
    }

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
    /* Whatever the kernel left in the D-cache for these pages (zeroing, a
       previous owner) must not be written back over GPU output later: write
       back and drop the whole data cache once (c7,c14,0), then every later
       CPU access to the arena does its own line maintenance. QEMU has no
       caches, so only hardware can show this matters. */
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 0\n\tmcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
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

void shim_gpu_thread_poll(void);

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
        IOSleep(1);                     /* a GL request or a mirror frame owns the GPU */
    run(&r, (uint32_t)proc_selfpid());
    busy = 0;
    shim_gpu_thread_poll();
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

/* ------------------------------------------------------------------------ */
/* GPU display mirror                                                       */
/* ------------------------------------------------------------------------ */

#define WD_COUNT   (*(vu32 *)(PRIV_VA + 0x624))     /* watchdog, timer mode: counts down */
#define TICK_HZ    1047311u
#define GUARD      4096u

static uint32_t m_ticks(void) { return ~WD_COUNT; }
static void m_to_gpu(void *p, uint32_t len) { dc_clean(p, len); }
static void m_from_gpu(void *p, uint32_t len) { dc_inval(p, len); }

static pm_plat_t mplat;
static pica_mirror_t mir;
static volatile uint32_t m_state;           /* GM_ST_* (the IRQ side only acts on ACTIVE) */
static uint32_t m_tried, m_vbl, m_fb[4], m_nsets;

static void m_set(uint32_t state, uint32_t why)
{
    __asm__ volatile("" ::: "memory");      /* mir, m_fb, m_nsets are complete before the IRQ side may look */
    m_state = state;
    GM->state = state;
    GM->reason = why;
    gm_publish();
}

/* An LCD framebuffer the ARM9 reported: in VRAM, 8-byte aligned, clear of
   /vram (iOS) and of the mirror's own texture and render targets. */
static int fb_ok(uint32_t pa, uint32_t len)
{
    if (!pa || (pa & 7) || pa < PICA_VRAM_PA || pa + len > PICA_VRAM_PA + PICA_VRAM_SIZE)
        return 0;
    if (pa < GM_IOS_VRAM_HI)
        return 0;
    return pa + len <= GM_VRAM_LO || pa >= GM_VRAM_HI;
}

/* Thread context. Brings the mirror up once the ARM9 asks for it. */
void shim_gpu_thread_poll(void)
{
    uint32_t src_pa = 0, out_pa = 0, bad0 = 0, fbad[4] = { 0, 0, 0, 0 }, t0, i;
    int v;

    if (m_tried || !gm_valid())
        return;
    if (!GM->gpu_enable || !GM->mirror_enable) {
        m_tried = 1;
        k_printf("[gpu] display mirror off in boot.cfg (gpu=%u gpu_mirror=%u): the ARM9 copies the screen\n",
                 GM->gpu_enable, GM->mirror_enable);
        m_set(GM_ST_DISABLED, GM_WHY_CFG);
        return;
    }
    if (!GM->want) {
        if (GM->state != GM_ST_WAIT)
            m_set(GM_ST_WAIT, GM_WHY_NONE);
        return;
    }
    if (!take())
        return;                         /* GL is using the GPU: next call */
    m_tried = 1;
    m_set(GM_ST_TEST, GM_WHY_NONE);
    if (!bringup_state)
        bringup();
    if (drv.state != PICA_OK) {
        k_printf("[gpu] display mirror: GPU not usable (state %u): CPU copy stays\n", drv.state);
        m_set(GM_ST_FAILED, GM_WHY_NO_GPU);
        goto out;
    }
    mplat.rd = p_rd;
    mplat.wr = p_wr;
    mplat.ticks = m_ticks;
    mplat.tick_hz = TICK_HZ;
    mplat.to_gpu = m_to_gpu;
    mplat.from_gpu = m_from_gpu;
    mplat.log = k_printf;
    if (pm_init(&mir, &mplat, (const void *)GM_BLOB_VA, GM_BLOB_PA)) {
        k_printf("[gpu] display mirror: no valid gpumirror.bin at PA 0x%08x (magic 0x%08x): CPU copy stays\n",
                 GM_BLOB_PA, *(vu32 *)GM_BLOB_VA);
        m_set(GM_ST_FAILED, GM_WHY_NO_BLOB);
        goto out;
    }
    gm_fetch();
    m_fb[0] = GM->top_fb[0];
    m_fb[1] = GM->top_fb[1];
    m_fb[2] = GM->bot_fb[0];
    m_fb[3] = GM->bot_fb[1];
    m_nsets = m_fb[1] && m_fb[3] && (m_fb[1] != m_fb[0] || m_fb[3] != m_fb[2]) ? 2 : 1;
    k_printf("[gpu] display mirror: LCD framebuffers top 0x%08x/0x%08x bottom 0x%08x/0x%08x (%u set%s); "
             "texture 0x%08x, targets 0x%08x/0x%08x..0x%08x\n", m_fb[0], m_fb[1], m_fb[2], m_fb[3], m_nsets,
             m_nsets > 1 ? "s" : "", GM_TEX_PA, GM_RT_TOP_PA, GM_RT_BOT_PA, GM_VRAM_HI);
    for (i = 0; i < 2 * m_nsets; i++)
        if (!fb_ok(m_fb[i % 2 * 2 + i / 2], i % 2 ? GM_BOT_FB_BYTES : GM_TOP_FB_BYTES)) {
            k_printf("[gpu] display mirror: framebuffer 0x%08x is not where the mirror can write it: CPU copy stays\n",
                     m_fb[i % 2 * 2 + i / 2]);
            m_set(GM_ST_FAILED, GM_WHY_LAYOUT);
            goto out;
        }
    if (pica_drv_alloc(&drv, 0, GM_SRC_W * GM_SRC_H * 4, 0, &src_pa) != PICA_OK ||
        pica_drv_alloc(&drv, 0, GM_TOP_FB_BYTES + GM_BOT_FB_BYTES + 2 * GUARD, 0, &out_pa) != PICA_OK) {
        k_printf("[gpu] display mirror: no arena memory for the self-test: CPU copy stays\n");
        m_set(GM_ST_FAILED, GM_WHY_NO_MEM);
        goto out;
    }
    t0 = m_ticks();
    v = pm_selftest(&mir, drv.va + (src_pa - drv.pa), src_pa, drv.va + (out_pa - drv.pa), out_pa, GUARD,
                    &bad0, fbad);
    GM->selftest_ticks = m_ticks() - t0;
    GM->selftest_bad = bad0;
    for (i = 0; i < 4; i++)
        GM->first_bad[i] = fbad[i];
    if (v < 0) {
        k_printf("[gpu] display mirror: self-test FAILED (%u bytes differ as modelled): CPU copy stays\n", bad0);
        m_set(GM_ST_FAILED, GM_WHY_SELFTEST);
        goto out;
    }
    mir.variant = (uint32_t)v;
    mir.frames = 0;
    mir.timeouts = 0;
    for (i = 0; i < 3; i++)
        mir.m_stage[i] = 0;
    mir.m_frame = mir.sum_frame = 0;
    GM->variant = (uint32_t)v;
    k_printf("[gpu] display mirror: self-test passed (variant %u) in %u us: the PICA200 copies the screen now "
             "(every %u. VBlank)\n", v, (GM->selftest_ticks * 1000u) / (TICK_HZ / 1000u), GM->vbl_div ? GM->vbl_div : 1);
    m_set(GM_ST_ACTIVE, GM_WHY_NONE);
out:
    if (src_pa)
        pica_drv_free(&drv, 0, src_pa);
    if (out_pa)
        pica_drv_free(&drv, 0, out_pa);
    busy = 0;
}

static void m_stats(void)
{
    GM->frames = mir.frames;
    GM->timeouts = mir.timeouts;
    GM->t_in = mir.t_stage[0];
    GM->t_3d = mir.t_stage[1];
    GM->t_out = mir.t_stage[2];
    GM->t_frame = mir.t_frame;
    GM->m_in = mir.m_stage[0];
    GM->m_3d = mir.m_stage[1];
    GM->m_out = mir.m_stage[2];
    GM->m_frame = mir.m_frame;
    GM->sum_frame = mir.sum_frame;
}

/* a frame operation came back from pm_poll(): bookkeeping (IRQ context) */
static void m_after(int st)
{
    if (st == PM_BUSY)
        return;
    busy = 0;
    m_stats();
    if (st == PM_TIMEOUT) {
        /* the GPU did not finish: stop mirroring for good; the ARM9 notices
           (frames stop) and copies with the CPU again */
        GM->first_bad[3] = mir.why_timeout;
        m_set(GM_ST_FAILED, GM_WHY_TIMEOUT);
        return;
    }
    gm_publish();
}

/* IRQ mode, IRQs masked, private stack (entry.S). id = GIC interrupt ID. */
void shim_gpu_irq(uint32_t id)
{
    uint32_t t0, spin;
    vu32 *clcd;
    pm_frame_t f;

    if (m_state != GM_ST_ACTIVE)
        return;
    t0 = m_ticks();
    spin = GM->spin_us * (TICK_HZ / 1000u) / 1000u;
    if (busy == 2) {                    /* a frame in flight: next operation if the last one is done */
        GM->steps++;
        m_after(pm_poll(&mir, spin));
        goto out;
    }
    if (id != PICA_IRQ_PDC0 || ++m_vbl < (GM->vbl_div ? GM->vbl_div : 1))
        return;
    m_vbl = 0;
    gm_fetch();
    if (GM->ack_seq != GM->seq) {
        GM->ack_seq = GM->seq;          /* nothing built from an older line 0 is in flight */
        gm_publish();
    }
    if (!GM->want)
        return;
    if (busy) {
        GM->busy_skips++;
        return;
    }
    /* the iOS framebuffer as AppleM2CLCD scans it out (fake CLCD page) */
    clcd = (vu32 *)(cfg_clcd_pa - AXI_PA + AXI_VA);
    f.src_pa = clcd[0x24 / 4];
    if (!(clcd[0] & 1) || clcd[0x28 / 4] != GM_SRC_W || clcd[0x30 / 4] != (GM_SRC_W << 16 | GM_SRC_H) ||
        f.src_pa < PICA_VRAM_PA || f.src_pa + GM_SRC_W * GM_SRC_H * 4 > GM_IOS_VRAM_HI || (f.src_pa & 7)) {
        GM->reason = GM_WHY_GEOMETRY;   /* skipped; the ARM9 copies while frames stand still */
        return;
    }
    f.mode = GM->mode;
    f.nsets = m_nsets;
    f.top[0] = m_fb[0];
    f.top[1] = m_fb[1];
    f.bot[0] = m_fb[2];
    f.bot[1] = m_fb[3];
    busy = 2;
    pm_begin(&mir, &f);
    GM->steps++;
    m_after(pm_poll(&mir, spin));
out:
    GM->sum_irq += m_ticks() - t0;
}
