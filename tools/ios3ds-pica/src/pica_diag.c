/*
 * ios3ds-pica: staged GPU diagnostics (include/pica_diag.h). One hardware
 * run should say which part of the PICA200 path works: every step logs the
 * registers it waited on, the GPU's own memory-traffic counters (GX +0x70..
 * +0xBC, libn3ds gx.h: undocumented but present on hardware, zero in our
 * model) and how many words came out right. Freestanding, like pica_drv.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pica_diag.h"
#include "pica_regs.h"

#define GXO(word)   ((word) * 4u)
#define P3DO(reg)   (0x1000u + (reg) * 4u)
#define VW(e, pa)   ((volatile uint32_t *)((e)->vram + ((pa) - PD_VRAM_PA)))

/* GX traffic counters, word index from +0x70 (libn3ds GxRegs) */
enum { C_EXT_RD, C_EXT_WR, C_VA_RD, C_VA_WR, C_VB_RD, C_VB_WR, C_VTX_RD, C_TEX_RD, C_Z_RD, C_Z_WR,
       C_COL_RD, C_COL_WR, C_PDC0_RD, C_PDC1_RD, C_PPF_RD, C_PPF_WR, C_FILL0_WR, C_FILL1_WR,
       C_CPU_VRAM_RD, C_CPU_VRAM_WR, C_N };

static void counters(const pd_env_t *e, uint32_t c[C_N])
{
    uint32_t i;
    for (i = 0; i < C_N; i++)
        c[i] = e->p->rd(0x70 + 4 * i);
}

static uint32_t morton(uint32_t x, uint32_t y)
{
    static const uint8_t xl[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint8_t yl[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };
    return xl[x & 7] + yl[y & 7] + (x & ~7u) * 8;
}
/* word index of pixel (x, stored row y) in an 8x8-tiled 32-bit image */
static uint32_t tw(uint32_t x, uint32_t y, uint32_t width) { return morton(x, y) + (y & ~7u) * width; }

/* position of the i-th word of tile 0 */
static void demorton(uint32_t i, uint32_t *x, uint32_t *y)
{
    *x = (i & 1) | (i >> 1 & 2) | (i >> 2 & 4);
    *y = (i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4);
}

/* ticks until (reg & mask), or ~0 on timeout (100 ms) */
static uint32_t wait_set(const pd_env_t *e, uint32_t off, uint32_t mask, uint32_t *last)
{
    uint32_t t0 = e->p->ticks(), lim = e->p->tick_hz / 10, v;
    for (;;) {
        v = e->p->rd(off);
        if (v & mask) {
            *last = v;
            return e->p->ticks() - t0;
        }
        if (e->p->ticks() - t0 > lim) {
            *last = v;
            return ~0u;
        }
    }
}

static uint32_t pattern(uint32_t i) { return i * 0x9E3779B1u ^ 0x13579BDFu; }

void pd_dump_regs(const pd_env_t *e, const char *when)
{
    const pm_plat_t *p = e->p;
    uint32_t c[C_N];
    p->log("[gpu-diag] %s: rev %08x clk %08x psc_vram %08x stat %08x prio %08x %08x unk58 %08x unkD0 %08x\n", when,
           p->rd(0x00), p->rd(0x04), p->rd(0x30), p->rd(0x34), p->rd(0x50), p->rd(0x54), p->rd(0x58), p->rd(0xD0));
    p->log("[gpu-diag] %s: psc0 %08x %08x %08x %08x psc1 %08x %08x %08x %08x\n", when,
           p->rd(0x10), p->rd(0x14), p->rd(0x18), p->rd(0x1C), p->rd(0x20), p->rd(0x24), p->rd(0x28), p->rd(0x2C));
    p->log("[gpu-diag] %s: ppf in %08x out %08x od %08x id %08x fl %08x u14 %08x cnt %08x irq %08x\n", when,
           p->rd(0xC00), p->rd(0xC04), p->rd(0xC08), p->rd(0xC0C), p->rd(0xC10), p->rd(0xC14), p->rd(0xC18),
           p->rd(0xC1C));
    p->log("[gpu-diag] %s: p3d irq ack %08x cmp %08x mask %08x autostop %08x draw_func0 %08x\n", when,
           p->rd(P3DO(P3D_IRQ_ACK)), p->rd(P3DO(P3D_IRQ_CMP)), p->rd(P3DO(P3D_IRQ_MASK)),
           p->rd(P3DO(P3D_IRQ_AUTOSTOP)), p->rd(P3DO(P3D_START_DRAW_FUNC0)));
    counters(e, c);
    p->log("[gpu-diag] %s: counters ext r/w %u/%u vramA r/w %u/%u vramB r/w %u/%u vtx %u tex %u z r/w %u/%u "
           "col r/w %u/%u ppf r/w %u/%u fill0/1 %u/%u cpu-vram r/w %u/%u\n", when,
           c[C_EXT_RD], c[C_EXT_WR], c[C_VA_RD], c[C_VA_WR], c[C_VB_RD], c[C_VB_WR], c[C_VTX_RD], c[C_TEX_RD],
           c[C_Z_RD], c[C_Z_WR], c[C_COL_RD], c[C_COL_WR], c[C_PPF_RD], c[C_PPF_WR], c[C_FILL0_WR], c[C_FILL1_WR],
           c[C_CPU_VRAM_RD], c[C_CPU_VRAM_WR]);
}

/* One PSC fill of 4 KiB with a 32-bit value, into VRAM or FCRAM. */
static int fill_test(const pd_env_t *e, const char *when, int fcram)
{
    const pm_plat_t *p = e->p;
    uint32_t pa = fcram ? e->fc_pa : GD_FILL_PA, val = fcram ? 0xC0DE0F0Cu : 0xC0DE0A11u, n = 1024, i, ok = 0;
    uint32_t c0[C_N], c1[C_N], t, last, other = 0;
    volatile uint32_t *w = fcram ? (volatile uint32_t *)e->fc_va : VW(e, pa);
    for (i = 0; i < n; i++)
        w[i] = 0x55AA55AAu;
    if (fcram)
        p->to_gpu(e->fc_va, n * 4);
    counters(e, c0);
    p->wr(GXO(GX_PSC_FILL0) + 0, pa >> 3);
    p->wr(GXO(GX_PSC_FILL0) + 4, (pa + n * 4) >> 3);
    p->wr(GXO(GX_PSC_FILL0) + 8, val);
    p->wr(GXO(GX_PSC_FILL0) + 12, PSC_FILL_32 | PSC_FILL_START);
    t = wait_set(e, GXO(GX_PSC_FILL0) + 12, PSC_FILL_DONE, &last);
    counters(e, c1);
    p->wr(GXO(GX_PSC_FILL0) + 12, 0);
    if (fcram)
        p->from_gpu(e->fc_va, n * 4);
    for (i = 0; i < n; i++) {
        if (w[i] == val)
            ok++;
        else if (w[i] != 0x55AA55AAu)
            other++;
    }
    p->log("[gpu-diag] %s: PSC fill %s PA %08x: %s after %u ticks (ctrl %08x, stat %08x); %u/%u words filled, "
           "%u other; fill0 +%u ext-wr +%u vramB-wr +%u; word0 %08x\n", when, fcram ? "FCRAM" : "VRAM ", pa,
           t == ~0u ? "NO DONE BIT" : "done", t == ~0u ? 0 : t, last, p->rd(0x34), ok, n, other,
           c1[C_FILL0_WR] - c0[C_FILL0_WR], c1[C_EXT_WR] - c0[C_EXT_WR], c1[C_VB_WR] - c0[C_VB_WR], w[0]);
    return ok == n;
}

uint32_t pd_fill_tests(const pd_env_t *e, const char *when)
{
    uint32_t r = 0;
    if (fill_test(e, when, 0))
        r |= PD_FILL_VRAM;
    if (fill_test(e, when, 1))
        r |= PD_FILL_FCRAM;
    return r;
}

static uint32_t run_list(const pd_env_t *e, uint32_t pa, uint32_t bytes, uint32_t *last)
{
    const pm_plat_t *p = e->p;
    uint32_t i;
    p->wr(P3DO(P3D_IRQ_ACK), 0);
    for (i = 0; i < 100000 && (p->rd(GXO(GX_PSC_IRQ_STAT)) & GX_STAT_P3D); i++)
        ;
    p->wr(P3DO(P3D_CMDBUF_SIZE0), bytes >> 3);
    p->wr(P3DO(P3D_CMDBUF_ADDR0), pa >> 3);
    p->wr(P3DO(P3D_CMDBUF_JUMP0), 1);
    return wait_set(e, GXO(GX_PSC_IRQ_STAT), GX_STAT_P3D, last);
}

static uint32_t ppf(const pd_env_t *e, uint32_t src, uint32_t dst, uint32_t dim, uint32_t flags, uint32_t *last)
{
    const pm_plat_t *p = e->p;
    uint32_t t;
    p->wr(GXO(GX_PPF_IN_ADDR), src >> 3);
    p->wr(GXO(GX_PPF_OUT_ADDR), dst >> 3);
    p->wr(GXO(GX_PPF_DT_INDIM), dim);
    p->wr(GXO(GX_PPF_DT_OUTDIM), dim);
    p->wr(GXO(GX_PPF_FLAGS), flags);
    p->wr(GXO(GX_PPF_UNK14), 0);
    p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    t = wait_set(e, GXO(GX_PPF_CNT), PPF_CNT_DONE, last);
    p->wr(GXO(GX_PPF_CNT), 0);
    return t;
}

static void say_draw(const pd_env_t *e, const char *what, uint32_t t, uint32_t last, const uint32_t *c0,
                     const uint32_t *c1, uint32_t ok, uint32_t n, uint32_t first_i, uint32_t got, uint32_t want)
{
    e->p->log("[gpu-diag] %s: P3D %s after %u ticks (stat %08x); %u/%u words right; vtx +%u tex +%u col-wr +%u "
              "ext-wr +%u; first wrong word %08x (index, or y<<16|x): %08x, expected %08x\n", what, t == ~0u ? "NO END MARKER" : "done",
              t == ~0u ? 0 : t, last, ok, n, c1[C_VTX_RD] - c0[C_VTX_RD], c1[C_TEX_RD] - c0[C_TEX_RD],
              c1[C_COL_WR] - c0[C_COL_WR], c1[C_EXT_WR] - c0[C_EXT_WR], first_i, got, want);
}

uint32_t pd_gpu_tests(const pd_env_t *e)
{
    const pm_plat_t *p = e->p;
    const gm_blob_t *b = (const gm_blob_t *)e->blob_va;
    uint32_t r = 0, i, n, ok, t, last, first, got, want, c0[C_N], c1[C_N];
    uint32_t *fw = (uint32_t *)e->fc_va;
    volatile uint32_t *rt = VW(e, GD_RT_PA), *tex = VW(e, GD_TEX_PA);

    /* 1. PPF: FCRAM linear -> VRAM tiled -> FCRAM linear, 64x64 RGBA8 */
    n = 64 * 64;
    for (i = 0; i < n; i++)
        fw[i] = pattern(i);
    for (i = 0; i < n; i++)
        fw[n + i] = 0;
    p->to_gpu(fw, 2 * n * 4);
    counters(e, c0);
    t = ppf(e, e->fc_pa, GD_PPF_PA, 64u << 16 | 64u,
            PPF_OUT_TILED | PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8), &last);
    i = t == ~0u ? ~0u : ppf(e, GD_PPF_PA, e->fc_pa + n * 4, 64u << 16 | 64u,
                             PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8), &last);
    counters(e, c1);
    p->from_gpu(fw + n, n * 4);
    for (ok = 0, first = ~0u, got = want = 0, i = 0; i < n; i++)
        if (fw[n + i] == pattern(i))
            ok++;
        else if (first == ~0u) {
            first = i;
            got = fw[n + i];
            want = pattern(i);
        }
    /* the tiled copy in VRAM: word 1 of a tile is pixel (1, 0) */
    p->log("[gpu-diag] PPF FCRAM->VRAM(tiled)->FCRAM: %u/%u words back; ppf r/w +%u/+%u; vram tile word1 %08x "
           "(want %08x)%s\n", ok, n, c1[C_PPF_RD] - c0[C_PPF_RD], c1[C_PPF_WR] - c0[C_PPF_WR], VW(e, GD_PPF_PA)[1],
           pattern(1), t == ~0u ? "; TIMEOUT" : "");
    if (ok == n)
        r |= PD_PPF;

    /* 2. P3D: a constant-colour quad over a 64x64 VRAM target */
    n = GD_RT_W * GD_RT_W;
    for (i = 0; i < n; i++)
        rt[i] = 0;
    counters(e, c0);
    t = run_list(e, e->blob_pa + b->diag_off[0], b->diag_bytes[0], &last);
    counters(e, c1);
    for (ok = 0, first = ~0u, got = 0, i = 0; i < n; i++)
        if (rt[i] == GD_SOLID_WORD)
            ok++;
        else if (first == ~0u) {
            first = i;
            got = rt[i];
        }
    say_draw(e, "solid quad -> VRAM", t, last, c0, c1, ok, n, first, got, GD_SOLID_WORD);
    if (ok == n)
        r |= PD_SOLID_VRAM;

    /* 3. the same list rendering into FCRAM (patched target address) */
    {
        uint32_t *patch = (uint32_t *)(e->blob_va + b->diag_patch), old = *patch, rpa = e->fc_pa + 0x8000;
        uint32_t *frt = (uint32_t *)(e->fc_va + 0x8000);
        for (i = 0; i < n; i++)
            frt[i] = 0;
        p->to_gpu(frt, n * 4);
        *patch = rpa >> 3;
        p->to_gpu(patch, 4);
        counters(e, c0);
        t = run_list(e, e->blob_pa + b->diag_off[0], b->diag_bytes[0], &last);
        counters(e, c1);
        *patch = old;
        p->to_gpu(patch, 4);
        p->from_gpu(frt, n * 4);
        for (ok = 0, first = ~0u, got = 0, i = 0; i < n; i++)
            if (frt[i] == GD_SOLID_WORD)
                ok++;
            else if (first == ~0u) {
                first = i;
                got = frt[i];
            }
        say_draw(e, "solid quad -> FCRAM", t, last, c0, c1, ok, n, first, got, GD_SOLID_WORD);
        if (ok == n)
            r |= PD_SOLID_FCRAM;
    }

    /* 4./5. a 64x64 and a 48x40 texture drawn 1:1 (nearest, replace) */
    for (int k = 1; k <= 2; k++) {
        uint32_t tw_ = k == 1 ? GD_RT_W : GD_NPOT_W, th = k == 1 ? GD_RT_W : GD_NPOT_H, x, y;
        for (i = 0; i < tw_ * th; i++)
            tex[i] = pattern(i + 77);
        for (i = 0; i < n; i++)
            rt[i] = 0;
        counters(e, c0);
        t = run_list(e, e->blob_pa + b->diag_off[k], b->diag_bytes[k], &last);
        counters(e, c1);
        /* GL row y of the texture is stored row th-1-y; of the target 63-y */
        for (ok = 0, first = ~0u, got = want = 0, y = 0; y < th; y++)
            for (x = 0; x < tw_; x++) {
                uint32_t wv = tex[tw(x, th - 1 - y, tw_)], gv = rt[tw(x, GD_RT_W - 1 - y, GD_RT_W)];
                if (wv == gv)
                    ok++;
                else if (first == ~0u) {
                    first = y << 16 | x;
                    got = gv;
                    want = wv;
                }
            }
        say_draw(e, k == 1 ? "texture 64x64 1:1" : "texture 48x40 (NPOT) 1:1", t, last, c0, c1, ok, tw_ * th,
                 first, got, want);
        if (ok == tw_ * th)
            r |= k == 1 ? PD_TEX_POT : PD_TEX_NPOT;
    }
    return r;
}

/* After a failed mirror self-test: what is in the texture and the top target? */
uint32_t pd_dump_mirror(const pd_env_t *e)
{
    const pm_plat_t *p = e->p;
    uint32_t i, x, y, sx, sy, got[8], want[8], same = 0;
    for (i = 0; i < 8; i++) {
        demorton(i, &x, &y);
        got[i] = VW(e, GM_TEX_PA)[i];
        want[i] = pm_pattern(x, y);
        same += got[i] == want[i];
    }
    p->log("[gpu-diag] mirror texture tile 0: %08x %08x %08x %08x %08x %08x %08x %08x\n", got[0], got[1], got[2],
           got[3], got[4], got[5], got[6], got[7]);
    p->log("[gpu-diag]   expected (raw copy): %08x %08x %08x %08x %08x %08x %08x %08x\n", want[0], want[1], want[2],
           want[3], want[4], want[5], want[6], want[7]);
    for (i = 0; i < 8; i++) {
        demorton(i, &x, &y);
        got[i] = VW(e, GM_RT_TOP_PA)[3 * GM_RT_W * 8 + i];         /* tile row 3: stored rows 24..31 */
        want[i] = 0;
        if (gm_map(0, x, 24 + y, &sx, &sy)) {
            uint32_t c = pm_pattern(sx, sy);
            want[i] = (c >> 16 & 0xFF) << 24 | (c >> 8 & 0xFF) << 16 | (c & 0xFF) << 8 | 0xFF;
        }
        same += got[i] == want[i];
    }
    p->log("[gpu-diag] mirror top target (rows 24-31, tile 0): %08x %08x %08x %08x %08x %08x %08x %08x\n", got[0],
           got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
    p->log("[gpu-diag]   expected: %08x %08x %08x %08x %08x %08x %08x %08x\n", want[0], want[1], want[2], want[3],
           want[4], want[5], want[6], want[7]);
    return same;
}
