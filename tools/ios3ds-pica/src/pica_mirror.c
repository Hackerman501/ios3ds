/*
 * ios3ds-pica: the GPU display mirror engine (include/pica_mirror.h).
 * Freestanding: no libc, no floating point. Register sequences are the
 * driver core's (pica_drv.c), split into "start" and "is it done yet" so the
 * kernel shim can run them from interrupts without waiting.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pica_mirror.h"
#include "pica_regs.h"

#define GXO(word)   ((word) * 4u)
#define P3DO(reg)   (0x1000u + (reg) * 4u)

int pm_init(pica_mirror_t *m, const pm_plat_t *p, void *blob, uint32_t blob_pa, uint32_t tex_pa)
{
    const gm_blob_t *b = (const gm_blob_t *)blob;
    uint32_t i;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < sizeof *m; i++)
        z[i] = 0;
    m->p = p;
    m->timeout_ticks = p->tick_hz / 5;          /* 200 ms per operation */
    if (b->magic != GM_BLOB_MAGIC || b->version != GM_BLOB_VERSION || b->blob_pa != blob_pa ||
        b->size > GM_BLOB_MAX || b->scr_pa != GM_SCR_PA || b->rt_top_pa != GM_RT_TOP_PA ||
        b->rt_bot_pa != GM_RT_BOT_PA || b->src_wh != (GM_SRC_H << 16 | GM_SRC_W))
        return -1;
    for (i = 0; i < 2; i++) {
        if (!b->list_bytes[i] || (b->list_bytes[i] & 15) || (b->list_off[i] & 15) ||
            b->list_off[i] + b->list_bytes[i] > b->size)
            return -1;
        m->list_pa[i] = blob_pa + b->list_off[i];
        m->list_bytes[i] = b->list_bytes[i];
    }
    /* point both lists at the texture (in the arena: FCRAM) */
    for (i = 0; i < 2; i++) {
        uint32_t *w = (uint32_t *)((uint8_t *)blob + b->tex_patch[i]);
        if (b->tex_patch[i] < b->list_off[i] || b->tex_patch[i] >= b->list_off[i] + b->list_bytes[i] || (tex_pa & 7))
            return -1;
        *w = tex_pa >> 3;
        p->to_gpu(w, 4);
    }
    m->tex_pa = tex_pa;
    return 0;
}

static void kick(pica_mirror_t *m, uint32_t n)
{
    const pm_plat_t *p = m->p;
    uint32_t i;
    if (m->op[n].kind == PM_OP_PPF) {
        p->wr(GXO(GX_PPF_IN_ADDR), m->op[n].a >> 3);
        p->wr(GXO(GX_PPF_OUT_ADDR), m->op[n].c >> 3);
        p->wr(GXO(GX_PPF_DT_INDIM), m->op[n].b);
        p->wr(GXO(GX_PPF_DT_OUTDIM), m->op[n].d);
        p->wr(GXO(GX_PPF_FLAGS), m->op[n].e);
        p->wr(GXO(GX_PPF_UNK14), 0);
        p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    } else if (m->op[n].kind == PM_OP_TEXCOPY) {
        p->wr(GXO(GX_PPF_IN_ADDR), m->op[n].a >> 3);
        p->wr(GXO(GX_PPF_OUT_ADDR), m->op[n].c >> 3);
        p->wr(GXO(GX_PPF_FLAGS), PPF_TEXCOPY);
        p->wr(GXO(GX_PPF_TC_LEN), m->op[n].b);
        p->wr(GXO(GX_PPF_TC_INDIM), m->op[n].d);
        p->wr(GXO(GX_PPF_TC_OUTDIM), m->op[n].e);
        p->wr(GXO(GX_PPF_CNT), PPF_CNT_START);
    } else {
        /* acknowledge the previous list's end marker; the flag drops at once */
        p->wr(P3DO(P3D_IRQ_ACK), 0);
        for (i = 0; i < 64 && (p->rd(GXO(GX_PSC_IRQ_STAT)) & GX_STAT_P3D); i++)
            ;
        p->wr(P3DO(P3D_CMDBUF_SIZE0), m->op[n].b >> 3);
        p->wr(P3DO(P3D_CMDBUF_ADDR0), m->op[n].a >> 3);
        p->wr(P3DO(P3D_CMDBUF_JUMP0), 1);
    }
}

static int finished(pica_mirror_t *m, uint32_t n)
{
    const pm_plat_t *p = m->p;
    if (m->op[n].kind != PM_OP_P3D) {
        if (!(p->rd(GXO(GX_PPF_CNT)) & PPF_CNT_DONE))
            return 0;
        p->wr(GXO(GX_PPF_CNT), 0);
        return 1;
    }
    return (p->rd(GXO(GX_PSC_IRQ_STAT)) & GX_STAT_P3D) != 0;
}

static void add(pica_mirror_t *m, uint8_t kind, uint8_t stage, uint32_t a, uint32_t b, uint32_t c,
                uint32_t d, uint32_t e)
{
    if (m->nops >= PM_MAX_OPS)
        return;
    m->op[m->nops].kind = kind;
    m->op[m->nops].stage = stage;
    m->op[m->nops].a = a;
    m->op[m->nops].b = b;
    m->op[m->nops].c = c;
    m->op[m->nops].d = d;
    m->op[m->nops].e = e;
    m->nops++;
}

void pm_begin(pica_mirror_t *m, const pm_frame_t *f)
{
    uint32_t k, v = m->variant, s = v & GM_VAR_SAMPLE_HALF ? 1 : 0;
    uint32_t in_dim = GM_SRC_H << 16 | GM_SRC_W;
    uint32_t out_flags = PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGB8) |
                         (v & GM_VAR_OUT_FLIP ? PPF_FLIP_V : 0);
    m->nops = 0;
    m->cur = 0;
    /* 1. iOS framebuffer -> tiled, 320 wide (same format, rows as stored),
       then each 8-row tile row (10240 bytes) into the 512-wide texture
       (16384 bytes per tile row): a raw copy with an output gap */
    add(m, PM_OP_PPF, 0, f->src_pa, in_dim, GM_SCR_PA, in_dim,
        PPF_OUT_TILED | PPF_IN_FMT(PICA_FB_RGBA8) | PPF_OUT_FMT(PICA_FB_RGBA8) |
        (v & GM_VAR_IN_FLIP ? PPF_FLIP_V : 0));
    add(m, PM_OP_TEXCOPY, 0, GM_SCR_PA, GM_SRC_W * GM_SRC_H * 4, m->tex_pa, GM_SRC_W * 8 * 4 / 16,
        ((GM_TEX_W - GM_SRC_W) * 8 * 4 / 16) << 16 | GM_SRC_W * 8 * 4 / 16);
    /* 2. both quads */
    add(m, PM_OP_P3D, 1, m->list_pa[s], m->list_bytes[s], 0, 0, 0);
    /* 3. render targets -> LCD framebuffers, RGBA8 -> BGR8. The bottom
       target follows the top one in VRAM, so when Luma's bottom framebuffer
       follows the top one as well, one transfer does both screens. */
    for (k = 0; k < f->nsets && k < 2; k++) {
        if (f->mode == 1 && !(v & GM_VAR_OUT_FLIP) && f->bot[k] == f->top[k] + GM_TOP_FB_BYTES) {
            uint32_t dim = (GM_RT_TOP_H + GM_RT_BOT_H) << 16 | GM_RT_W;
            add(m, PM_OP_PPF, 2, GM_RT_TOP_PA, dim, f->top[k], dim, out_flags);
            continue;
        }
        if (f->mode != 2)
            add(m, PM_OP_PPF, 2, GM_RT_TOP_PA, GM_RT_TOP_H << 16 | GM_RT_W, f->top[k],
                GM_RT_TOP_H << 16 | GM_RT_W, out_flags);
        if (f->mode != 0)
            add(m, PM_OP_PPF, 2, GM_RT_BOT_PA, GM_RT_BOT_H << 16 | GM_RT_W, f->bot[k],
                GM_RT_BOT_H << 16 | GM_RT_W, out_flags);
    }
    m->t_frame0 = m->t_op0 = m->t_stage0[0] = m->p->ticks();
    m->state = PM_BUSY;
    kick(m, 0);
}

int pm_poll(pica_mirror_t *m, uint32_t spin_ticks)
{
    uint32_t t0 = m->p->ticks(), now;
    if (m->state != PM_BUSY)
        return (int)m->state;
    for (;;) {
        now = m->p->ticks();
        if (finished(m, m->cur)) {
            uint32_t st = m->op[m->cur].stage;
            m->cur++;
            if (m->cur == m->nops || m->op[m->cur].stage != st) {
                uint32_t d = now - m->t_stage0[st];
                m->t_stage[st] = d;
                if (d > m->m_stage[st])
                    m->m_stage[st] = d;
                if (m->cur < m->nops)
                    m->t_stage0[m->op[m->cur].stage] = now;
            }
            if (m->cur == m->nops) {
                m->t_frame = now - m->t_frame0;
                if (m->t_frame > m->m_frame)
                    m->m_frame = m->t_frame;
                m->sum_frame += m->t_frame;
                m->frames++;
                m->state = PM_DONE;
                return PM_DONE;
            }
            m->t_op0 = now;
            kick(m, m->cur);
            continue;
        }
        if (now - m->t_op0 > m->timeout_ticks) {
            m->why_timeout = m->op[m->cur].kind != PM_OP_P3D ? m->p->rd(GXO(GX_PPF_CNT))
                                                              : m->p->rd(GXO(GX_PSC_IRQ_STAT));
            if (m->op[m->cur].kind != PM_OP_P3D)
                m->p->wr(GXO(GX_PPF_CNT), 0);
            m->timeouts++;
            m->state = PM_TIMEOUT;
            return PM_TIMEOUT;
        }
        if (now - t0 >= spin_ticks)
            return PM_BUSY;
    }
}

static int wait_frame(pica_mirror_t *m)
{
    int st;
    do
        st = pm_poll(m, m->timeout_ticks);
    while (st == PM_BUSY);
    return st;
}

int pm_clear_targets(pica_mirror_t *m)
{
    const pm_plat_t *p = m->p;
    uint32_t t0 = p->ticks(), ok = 0;
    p->wr(GXO(GX_PSC_FILL0) + 0, GM_RT_TOP_PA >> 3);
    p->wr(GXO(GX_PSC_FILL0) + 4, GM_RT_BOT_PA >> 3);
    p->wr(GXO(GX_PSC_FILL0) + 8, 0);
    p->wr(GXO(GX_PSC_FILL0) + 12, PSC_FILL_32 | PSC_FILL_START);
    p->wr(GXO(GX_PSC_FILL1) + 0, GM_RT_BOT_PA >> 3);
    p->wr(GXO(GX_PSC_FILL1) + 4, GM_VRAM_HI >> 3);
    p->wr(GXO(GX_PSC_FILL1) + 8, 0);
    p->wr(GXO(GX_PSC_FILL1) + 12, PSC_FILL_32 | PSC_FILL_START);
    while (p->ticks() - t0 < m->timeout_ticks) {
        ok = (p->rd(GXO(GX_PSC_FILL0) + 12) & PSC_FILL_DONE) && (p->rd(GXO(GX_PSC_FILL1) + 12) & PSC_FILL_DONE);
        if (ok)
            break;
    }
    p->wr(GXO(GX_PSC_FILL0) + 12, 0);
    p->wr(GXO(GX_PSC_FILL1) + 12, 0);
    return ok ? 0 : -1;
}

/* expected framebuffer byte k of out (top image, then bottom) */
static uint32_t expect(uint32_t off)
{
    uint32_t bottom = off >= GM_TOP_FB_BYTES, px = bottom ? (off - GM_TOP_FB_BYTES) / 3 : off / 3;
    uint32_t x, y, c = bottom ? (off - GM_TOP_FB_BYTES) % 3 : off % 3;
    if (!gm_map((int)bottom, px % GM_RT_W, px / GM_RT_W, &x, &y))
        return 0;
    return pm_pattern(x, y) >> (8 * c) & 0xFF;
}

static const char *const var_name[GM_VARIANTS] = {
    "as modelled", "sampling rounds", "texture rows flipped", "rounds + texture flipped",
    "target rows flipped", "rounds + target flipped", "both flipped", "rounds + both flipped",
};

int pm_selftest(pica_mirror_t *m, uint8_t *src_va, uint32_t src_pa, uint8_t *out_va, uint32_t out_pa,
                uint32_t guard, uint32_t *bad0, uint32_t first_bad[4])
{
    const pm_plat_t *p = m->p;
    uint32_t body = GM_TOP_FB_BYTES + GM_BOT_FB_BYTES, total = body + 2 * guard, x, y, i, v;
    uint32_t *s = (uint32_t *)src_va;
    pm_frame_t f;

    for (y = 0; y < GM_SRC_H; y++)
        for (x = 0; x < GM_SRC_W; x++)
            s[y * GM_SRC_W + x] = pm_pattern(x, y);
    p->to_gpu(src_va, GM_SRC_W * GM_SRC_H * 4);
    if (pm_clear_targets(m)) {
        p->log("[gpu] mirror self-test: memory fill of the render targets timed out (PSC0 0x%08x PSC1 0x%08x)\n",
               p->rd(GXO(GX_PSC_FILL0) + 12), p->rd(GXO(GX_PSC_FILL1) + 12));
        return -1;
    }
    f.src_pa = src_pa;
    f.mode = 1;
    f.nsets = 1;
    f.top[0] = f.top[1] = out_pa + guard;
    f.bot[0] = f.bot[1] = out_pa + guard + GM_TOP_FB_BYTES;
    for (v = 0; v < GM_VARIANTS; v++) {
        uint32_t bad = 0, guard_bad = 0, fb[4] = { 0, 0, 0, 0 };
        for (i = 0; i < total; i++)
            out_va[i] = 0xA5;
        p->to_gpu(out_va, total);
        m->variant = v;
        pm_begin(m, &f);
        if (wait_frame(m) != PM_DONE) {
            p->log("[gpu] mirror self-test: GPU operation %u of %u (%s) did not finish, register 0x%08x\n",
                   m->cur + 1, m->nops, m->op[m->cur].kind == PM_OP_P3D ? "P3D list" :
                   m->op[m->cur].kind == PM_OP_PPF ? "PPF transfer" : "PPF texture copy",
                   m->why_timeout);
            return -1;
        }
        p->from_gpu(out_va, total);
        for (i = 0; i < guard; i++)
            guard_bad += (out_va[i] != 0xA5) + (out_va[guard + body + i] != 0xA5);
        for (i = 0; i < body; i++) {
            uint32_t e = expect(i), g = out_va[guard + i];
            if (e != g) {
                if (!bad) {
                    fb[0] = i;
                    fb[1] = e;
                    fb[2] = g;
                    fb[3] = m->nops;
                }
                bad++;
            }
        }
        if (v == 0) {
            *bad0 = bad;
            for (i = 0; i < 4; i++)
                first_bad[i] = fb[i];
        }
        p->log("[gpu] mirror self-test variant %u (%s): %u of %u bytes differ, guard %s; "
               "GPU %u/%u/%u ticks (upload/draw/read-out)\n",
               v, var_name[v], bad, body, guard_bad ? "OVERWRITTEN" : "intact",
               m->t_stage[0], m->t_stage[1], m->t_stage[2]);
        if (bad) {
            uint32_t px = (fb[0] >= GM_TOP_FB_BYTES ? fb[0] - GM_TOP_FB_BYTES : fb[0]) / 3;
            p->log("[gpu]   first difference at byte %u (%s screen, fb row %u, pixel %u): expected 0x%02x, got 0x%02x\n",
                   fb[0], fb[0] >= GM_TOP_FB_BYTES ? "bottom" : "top", px / GM_RT_W, px % GM_RT_W, fb[1], fb[2]);
        }
        if (guard_bad) {
            p->log("[gpu]   the read-out wrote outside its framebuffer: GPU mirror unusable\n");
            return -1;
        }
        if (!bad)
            return (int)v;
    }
    return -1;
}
