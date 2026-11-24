/*
 * genmirror: build gpumirror.bin, the PICA200 command lists of the GPU
 * display mirror (boot/common/gpu_mirror.h, docs/pica200.md section 9).
 *
 *   genmirror out.bin
 *
 * Runs on the build host with the same encoders the GL back end uses
 * (pica_cmd/pica_state/pica_ff), so the kernel shim needs no floating point
 * and no encoder of its own: it only points the GPU at these lists. The
 * lists use fixed physical addresses (VRAM texture and render targets, the
 * blob's own vertex arrays at GM_BLOB_PA); only the texture's address (it is
 * allocated from the FCRAM arena) and the stereo eye's texture-coordinate
 * shifts (the slider position) are patched in by the shim.
 *
 * Four lists:
 *   [0] flat, texture coordinates for a GPU whose nearest sampling truncates
 *       (texel = floor(s * width), what Citra/Azahar and our model do)
 *   [1] flat, the same shifted by -1/2 texel (GM_VAR_SAMPLE_HALF)
 *   [2]/[3] the same two variants with two eyes. Each eye draws a baseline
 *       pass (all pixels, no shift) and three passes selected by the depth
 *       stamp in texture unit 1 (alpha test on the *unshifted* stamp), with
 *       texture unit 0 sampled at the class's eye offset:
 *         BG      left +k, right -k
 *         FRONT   left -k, right +k
 *         ALERT   left -2k, right +2k
 *       (out(x) = in(x - delta), boot/common/stereo.h). The left eye goes
 *       into GM_RT_TOP_PA, the right eye into the upload buffer, which is
 *       free after the PPF texture copy. The bottom screen is flat.
 *
 * Geometry. In the render targets' own terms (column q from memory x = 0,
 * stored row r from the first stored row) both screens are plain scalings
 * of the iOS picture (gm_map):
 *   top    240x400: q 0..240, r 20..380;  iOS x = 4/3 q - 1/2, iOS y = 4/3 (r - 20) - 1/2
 *   bottom 240x320: q 14..227, r 0..320;  iOS x = 3/2 q - 41/2, iOS y = 3/2 r - 1/2
 * At pixel centres (q, r = n + 1/2) those give floor() = the CPU copy's
 * integer maths, with at least 1/6 texel to spare on either side. The
 * margins are never drawn: the shim fills both targets with black once.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pica.h"
#include "gpu_mirror.h"
#include "stereo.h"

typedef struct {                /* = struct Vtx of the GL engine: the ff shader's input */
    float x, y, z, w;
    float c[4];
    float t0[2], t1[2];
    float fog;
} MVtx;
_Static_assert(sizeof(MVtx) == 52, "vertex layout");

static void quad(MVtx *v, float W, float H, float q0, float q1, float r0, float r1,
                 float ax, float bx, float ay, float by, float shift)
{
    /* corners: clip x = 2q/W - 1, clip y = 1 - 2r/H (stored row 0 = top, +y) */
    const float qs[6] = { q0, q1, q1, q0, q1, q0 }, rs[6] = { r0, r0, r1, r0, r1, r1 };
    for (int i = 0; i < 6; i++) {
        float X = ax * qs[i] + bx, Y = ay * rs[i] + by;     /* iOS pixel coordinates */
        memset(&v[i], 0, sizeof v[i]);
        v[i].x = 2.0f * qs[i] / W - 1.0f;
        v[i].y = 1.0f - 2.0f * rs[i] / H;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].c[0] = v[i].c[1] = v[i].c[2] = v[i].c[3] = 1.0f;
        /* GL t runs from the last stored texture row (t = 0) to the first;
           iOS row y is stored row y of the 512x512 texture, so t * 512 = 512 - Y.
           Unit 0 carries the (possibly shifted) colour sample; unit 1 reads the
           depth stamp at the output pixel and must never be shifted. */
        v[i].t0[0] = (X + shift) / (float)GM_TEX_W;
        v[i].t0[1] = ((float)GM_TEX_H - Y + shift) / (float)GM_TEX_H;
        v[i].t1[0] = X / (float)GM_TEX_W;
        v[i].t1[1] = ((float)GM_TEX_H - Y) / (float)GM_TEX_H;
        v[i].fog = 1.0f;
    }
}

/* what one draw in a list does: a render target, an optional texture, a TEV
   program, alpha test / texture-matrix shift, and a run of triangles */
typedef struct {
    uint32_t rt_pa, rt_w, rt_h;
    uint32_t tex_pa, tex_w, tex_h;      /* tex_pa 0 = untextured */
    int tev;                            /* 0 mirror swizzle, 1 constant, 2 replace texture,
                                           3 two-unit stereo swizzle, 4 black clear */
    uint32_t first, count;
    uint32_t alpha_ref;                 /* 0 = alpha test off */
    uint32_t shift_word;                /* IEEE bits of the added texture-s coordinate */
} Pass;

static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static void emit_program(pica_cmd_t *c, uint32_t vtx_pa, const Pass *p)
{
    pica_tex_t t;
    pica_tev_t tev[PICA_TEV_STAGES];
    pica_raster_t rs;
    pica_vtx_layout_t l;
    float clip[16];
    int i;

    /* Every list is self-contained: the shim may submit it at any time after
       a GL request, including after a stereo list left per-pass state behind. */
    pica_emit_base_state(c);
    pica_ff_emit_program(c);

    if (p->tex_pa) {
        memset(&t, 0, sizeof t);
        t.pa = p->tex_pa;
        t.width = (uint16_t)p->tex_w;
        t.height = (uint16_t)p->tex_h;
        t.fmt = PICA_TEX_RGBA8;
        t.mag = t.min = PICA_NEAREST;
        t.wrap_s = t.wrap_t = PICA_CLAMP_TO_EDGE;
        pica_emit_texture(c, 0, &t);
        if (p->tev == 3)
            pica_emit_texture(c, 1, &t);        /* same pixels, unshifted stamp */
        pica_emit_texunits(c, p->tev == 3 ? 3 : 1);
    } else {
        pica_emit_texunits(c, 0);
    }

    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&tev[i]);
    if (p->tev == 0 || p->tev == 3) {
        /* The texture holds the iOS bytes B,G,R,A, read as RGBA8 (bytes A,B,G,R):
           sample.a = B, .b = G, .g = R, .r = A. Rebuild the real colour:
             stage 0: (sample.g) * (1,0,0)            -> (R, 0, 0)
             stage 1: (sample.b) * (0,1,0) + previous -> (R, G, 0)
             stage 2: (sample.a) * (0,0,1) + previous -> (R, G, B)
           Multiplying by 0 or 255 and adding disjoint channels is exact.
           In stereo mode the stage-0 alpha is unit 1's red channel: the
           depth stamp at the output pixel (the alpha test selects the class). */
        static const uint8_t op[3] = { PICA_TEVOP_RGB_G, PICA_TEVOP_RGB_B, PICA_TEVOP_RGB_ALPHA };
        static const uint32_t k[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u }; /* R | G<<8 | B<<16 | A<<24 */
        for (i = 0; i < 3; i++) {
            tev[i].src_rgb[0] = PICA_TEVSRC_TEXTURE0;
            tev[i].src_rgb[1] = PICA_TEVSRC_CONSTANT;
            tev[i].src_rgb[2] = i ? PICA_TEVSRC_PREVIOUS : PICA_TEVSRC_CONSTANT;
            tev[i].op_rgb[0] = op[i];
            tev[i].op_rgb[1] = PICA_TEVOP_RGB_COLOR;
            tev[i].op_rgb[2] = PICA_TEVOP_RGB_COLOR;
            tev[i].func_rgb = i ? PICA_TEV_MULT_ADD : PICA_TEV_MODULATE;
            tev[i].color = k[i];
            tev[i].func_a = PICA_TEV_REPLACE;
            if (p->tev == 3 && i == 0) {
                /* the alpha test selects the depth class from the *unshifted*
                   stamp read through unit 1; later stages must pass it on */
                tev[i].src_a[0] = PICA_TEVSRC_TEXTURE1;
                tev[i].op_a[0] = PICA_TEVOP_A_R;
            } else if (p->tev == 3) {
                tev[i].src_a[0] = PICA_TEVSRC_PREVIOUS;
                tev[i].op_a[0] = PICA_TEVOP_A_ALPHA;
            } else {
                tev[i].src_a[0] = PICA_TEVSRC_CONSTANT;
                tev[i].op_a[0] = PICA_TEVOP_A_ALPHA;
            }
        }
    } else if (p->tev == 4) {
        tev[0].src_rgb[0] = PICA_TEVSRC_CONSTANT;
        tev[0].color = 0xFF000000u;             /* black, opaque */
    } else {
        tev[0].src_rgb[0] = tev[0].src_a[0] = p->tev == 1 ? PICA_TEVSRC_CONSTANT : PICA_TEVSRC_TEXTURE0;
        tev[0].color = GD_SOLID_TEV;
    }
    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(c, (unsigned)i, &tev[i]);

    pica_raster_defaults(&rs);
    rs.write_mask = PICA_WRITE_COLOR;
    pica_emit_raster(c, &rs);
    pica_ff_clip_matrix(clip, ident, ident);
    pica_ff_emit_matrices(c, clip, NULL, NULL, 1.0f);

    memset(&l, 0, sizeof l);
    l.count = 4;
    l.attr[0].type = PICA_FLOAT; l.attr[0].size = 4;
    l.attr[1].type = PICA_FLOAT; l.attr[1].size = 4;
    l.attr[2].type = PICA_FLOAT; l.attr[2].size = 2;
    l.attr[3].type = PICA_FLOAT; l.attr[3].size = 2;
    l.nbuffers = 1;
    l.buffer[0].stride = sizeof(MVtx);
    l.base_pa = vtx_pa;
    pica_emit_vtx_layout(c, &l);
}

static void emit_pass(pica_cmd_t *c, const Pass *p)
{
    pica_fb_t fb;
    float tex0[16];
    uint32_t at;

    /* per-pass state: alpha test compares TEV alpha (unit 1's stamp) and the
       texture matrix adds the eye shift to unit 0's s coordinate. */
    at = p->alpha_ref ? 1u | (PICA_EQUAL << 4) | (p->alpha_ref << 8) : 0;
    pica_cmd_write(c, P3D_FRAGOP_ALPHA_TEST, at);
    memcpy(tex0, ident, sizeof tex0);
    memcpy(&tex0[12], &p->shift_word, 4);
    pica_ff_emit_matrices(c, ident, tex0, NULL, 1.0f);

    memset(&fb, 0, sizeof fb);
    fb.color_fmt = PICA_CB_RGBA8;
    fb.width = (uint16_t)p->rt_w;
    fb.height = (uint16_t)p->rt_h;
    fb.color_pa = p->rt_pa;
    pica_emit_framebuffer(c, &fb);
    pica_emit_viewport(c, 0, 0, p->rt_w, p->rt_h);
    pica_emit_scissor(c, 0, 0, 0, 0, 0);
    pica_emit_draw_arrays(c, PICA_TRIANGLES, p->first, p->count);
    pica_emit_fb_flush(c);
}

static uint32_t build_list(uint32_t *buf, uint32_t cap, uint32_t vtx_pa, const Pass *passes, int n)
{
    pica_cmd_t c;
    int i;
    pica_cmd_init(&c, buf, cap);
    emit_program(&c, vtx_pa, &passes[0]);
    for (i = 0; i < n; i++)
        emit_pass(&c, &passes[i]);
    return pica_cmd_finish(&c);
}

/* GL-style quad over [x0,x1] x [y0,y1] of a W x H target (y up), texcoords
    0..1 over it */
static void gl_quad(MVtx *v, float W, float H, float x0, float x1, float y0, float y1)
{
    const float xs[6] = { x0, x1, x1, x0, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
    for (int i = 0; i < 6; i++) {
        memset(&v[i], 0, sizeof v[i]);
        v[i].x = 2.0f * xs[i] / W - 1.0f;
        v[i].y = 2.0f * ys[i] / H - 1.0f;
        v[i].w = 1.0f;
        v[i].c[0] = v[i].c[1] = v[i].c[2] = v[i].c[3] = 1.0f;
        v[i].t0[0] = (xs[i] - x0) / (x1 - x0);
        v[i].t0[1] = (ys[i] - y0) / (y1 - y0);
        v[i].fog = 1.0f;
    }
}

/* distinct IEEE words used as placeholders for the six slider-dependent
   shift uniforms; genmirror finds them in the finished list and records the
   offsets for the shim. They are NaN-ish and never reach the GPU unpatched. */
static uint32_t shift_word(int i) { return 0x7F100000u + (uint32_t)i; }

int main(int argc, char **argv)
{
    static uint8_t blob[GM_BLOB_MAX];
    gm_blob_t *h = (gm_blob_t *)blob;
    uint32_t off = (sizeof *h + 15) & ~15u, vtx_off[2], st_vtx_off[2], i;

    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: genmirror out.bin\n");
        return 2;
    }
    memset(blob, 0, sizeof blob);
    /* vertex arrays first (fixed size), lists after */
    for (i = 0; i < 2; i++) {
        MVtx v[12];
        float sh = i ? -0.5f : 0.0f;
        quad(v, GM_RT_W, GM_RT_TOP_H, 0, GM_RT_W, 20, 380, 4.0f / 3, -0.5f, 4.0f / 3, -20 * 4.0f / 3 - 0.5f, sh);
        quad(v + 6, GM_RT_W, GM_RT_BOT_H, 14, 227, 0, GM_RT_BOT_H, 1.5f, -20.5f, 1.5f, -0.5f, sh);
        vtx_off[i] = off;
        memcpy(blob + off, v, sizeof v);
        off = (off + sizeof v + 15) & ~15u;
    }
    for (i = 0; i < 2; i++) {
        const Pass passes[2] = {
            { GM_RT_TOP_PA, GM_RT_W, GM_RT_TOP_H, GM_TEX_PLACEHOLDER, GM_TEX_W, GM_TEX_H, 0, 0, 6, 0, 0 },
            { GM_RT_BOT_PA, GM_RT_W, GM_RT_BOT_H, GM_TEX_PLACEHOLDER, GM_TEX_W, GM_TEX_H, 0, 6, 6, 0, 0 },
        };
        uint32_t bytes = build_list((uint32_t *)(blob + off), (GM_BLOB_MAX - off) / 4, GM_BLOB_PA + vtx_off[i], passes, 2);
        if (!bytes) {
            fprintf(stderr, "genmirror: list %u does not fit\n", i);
            return 1;
        }
        h->list_off[i] = off;
        h->list_bytes[i] = bytes;
        {   /* the texture lives in the FCRAM arena: the shim patches this word */
            uint32_t k, n = 0;
            for (k = 0; k < bytes / 4; k++)
                if (((uint32_t *)(blob + off))[k] == GM_TEX_PLACEHOLDER >> 3) {
                    h->tex_patch[i] = off + k * 4;
                    n++;
                }
            if (n != 1) {
                fprintf(stderr, "genmirror: texture address found %u times in list %u\n", n, i);
                return 1;
            }
        }
        off = (off + bytes + 15) & ~15u;
    }
    /* two-eye lists: one vertex array each (top + bottom, plus a full-target
       quad to clear the right eye's render target before drawing into it) */
    for (i = 0; i < 2; i++) {
        MVtx v[18];
        float sh = i ? -0.5f : 0.0f;
        quad(v, GM_RT_W, GM_RT_TOP_H, 0, GM_RT_W, 20, 380, 4.0f / 3, -0.5f, 4.0f / 3, -20 * 4.0f / 3 - 0.5f, sh);
        quad(v + 6, GM_RT_W, GM_RT_BOT_H, 14, 227, 0, GM_RT_BOT_H, 1.5f, -20.5f, 1.5f, -0.5f, sh);
        gl_quad(v + 12, GM_RT_W, GM_RT_TOP_H, 0, GM_RT_W, 0, GM_RT_TOP_H);
        st_vtx_off[i] = off;
        memcpy(blob + off, v, sizeof v);
        off = (off + sizeof v + 15) & ~15u;
    }
    for (i = 0; i < 2; i++) {
        uint32_t bytes, k, n, eye, cls, vp;
        pica_cmd_t c;
        const uint32_t magic[3] = { SA_BG, SA_FRONT, SA_ALERT };
        uint32_t pa = GM_BLOB_PA + st_vtx_off[i];
        Pass top = { GM_RT_TOP_PA, GM_RT_W, GM_RT_TOP_H, GM_TEX_PLACEHOLDER, GM_TEX_W, GM_TEX_H, 3, 0, 6, 0, 0 };

        pica_cmd_init(&c, (uint32_t *)(blob + off), (GM_BLOB_MAX - off) / 4);
        emit_program(&c, pa, &top);              /* texture units, stereo TEV, layout */
        for (eye = 0; eye < 2; eye++) {
            if (eye) {
                /* The upload buffer is free after the PPF texture copy. Its
                   tiled layout as a 240x400 target has no useful margins, so
                   clear the whole target before the right-eye passes. */
                Pass clear = { GM_SCR_PA, GM_RT_W, GM_RT_TOP_H, 0, 0, 0, 4, 12, 6, 0, 0 };
                pica_cmd_write(&c, P3D_FRAGOP_ALPHA_TEST, 0);
                emit_pass(&c, &clear);
                emit_program(&c, pa, &top);     /* restore the stereo program */
            }
            for (cls = 0; cls < 4; cls++) {
                Pass p = top;
                p.rt_pa = eye ? GM_SCR_PA : GM_RT_TOP_PA;
                p.alpha_ref = cls ? magic[cls - 1] : 0;
                p.shift_word = cls ? shift_word(eye * 3 + cls - 1) : 0;
                emit_pass(&c, &p);
            }
        }
        {   /* bottom screen: one flat image, no eye offset */
            Pass p = top;
            p.rt_pa = GM_RT_BOT_PA;
            p.rt_h = GM_RT_BOT_H;
            p.first = 6;
            emit_pass(&c, &p);
        }
        bytes = pica_cmd_finish(&c);
        if (!bytes) {
            fprintf(stderr, "genmirror: stereo list %u does not fit\n", i);
            return 1;
        }
        h->st_list_off[i] = off;
        h->st_list_bytes[i] = bytes;
        n = 0;
        for (k = 0; k < bytes / 4; k++) {
            uint32_t w = ((uint32_t *)(blob + off))[k];
            if (w == GM_TEX_PLACEHOLDER >> 3) {
                if (n < 4) h->st_tex_patch[i][n] = off + k * 4;
                n++;
            }
            for (vp = 0; vp < 6; vp++)
                if (w == shift_word(vp))
                    h->st_shift_off[i][vp] = off + k * 4;
        }
        if (n != 4) {
            fprintf(stderr, "genmirror: stereo texture address found %u times in list %u\n", n, i);
            return 1;
        }
        for (vp = 0; vp < 6; vp++)
            if (!h->st_shift_off[i][vp]) {
                fprintf(stderr, "genmirror: stereo shift %u missing in list %u\n", vp, i);
                return 1;
            }
        off = (off + bytes + 15) & ~15u;
    }
    /* slider shift table: [k-1][6] IEEE float words of shift/512, in the same
       order as st_shift_off (BG/FRONT/ALERT per eye): out(x) = in(x-delta),
       so left = +k/-k/-2k and right = -k/+k/+2k. The shim copies one word
       into each pass's texture matrix. */
    {
        static const int8_t units[6] = { 1, -1, -2, -1, 1, 2 };
        uint32_t k, j;
        h->st_shift_tab = off;
        for (k = 1; k <= 4; k++)
            for (j = 0; j < 6; j++) {
                float f = (float)(units[j] * (int)k) / (float)GM_TEX_W;
                memcpy(blob + off, &f, 4);
                off += 4;
            }
        off = (off + 15) & ~15u;
    }
    /* Edge-padding table for the shifted samples: after the texture copy the
       shim replicates the x=319 texel into x=320..327 so the GPU's clamp at
       the 512-wide POT texture has the same result as the CPU copy's clamp
       at 320. One 8x8 tile row per entry: 8 source word indices, then the
       8 destination tile bases (the x offsets inside a tile are fixed). */
    {
        static const uint8_t spread[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
        uint32_t ty, dy;
        h->st_pad_off = off;
        for (ty = 0; ty < GM_SRC_H / 8; ty++)
            for (dy = 0; dy < 8; dy++) {
                uint32_t base = ty * (GM_TEX_W / 8) * 64;
                uint32_t src = base + (GM_SRC_W / 8 - 1) * 64 + spread[7] + (spread[dy] << 1);
                uint32_t dst = base + (GM_SRC_W / 8) * 64 + (spread[dy] << 1);
                memcpy(blob + off, &src, 4);
                off += 4;
                memcpy(blob + off, &dst, 4);
                off += 4;
            }
        off = (off + 15) & ~15u;
    }
    /* diagnostics (pica_diag.c): one solid quad, one 64x64 texture drawn 1:1
       and one 48x40 image in a padded 64x64 texture with scaled texcoords
       (what the GL back end does for NPOT images on the real GPU), all into a
       64x64 VRAM target */
    {
        MVtx v[12];
        uint32_t dv = off, k, w;
        int j;
        gl_quad(v, GD_RT_W, GD_RT_W, 0, GD_RT_W, 0, GD_RT_W);
        gl_quad(v + 6, GD_RT_W, GD_RT_W, 0, GD_NPOT_W, 0, GD_NPOT_H);
        for (j = 0; j < 6; j++) {
            v[6 + j].t0[0] *= (float)GD_NPOT_W / GD_RT_W;
            v[6 + j].t0[1] *= (float)GD_NPOT_H / GD_RT_W;
        }
        memcpy(blob + off, v, sizeof v);
        off = (off + sizeof v + 15) & ~15u;
        for (i = 0; i < 3; i++) {
            const Pass p = { GD_RT_PA, GD_RT_W, GD_RT_W, i ? GD_TEX_PA : 0, GD_RT_W, GD_RT_W,
                             i ? 2 : 1, i == 2 ? 6 : 0, 6, 0, 0 };
            uint32_t bytes = build_list((uint32_t *)(blob + off), (GM_BLOB_MAX - off) / 4, GM_BLOB_PA + dv, &p, 1);
            if (!bytes) {
                fprintf(stderr, "genmirror: diagnostic list %u does not fit\n", i);
                return 1;
            }
            h->diag_off[i] = off;
            h->diag_bytes[i] = bytes;
            if (i == 0) {
                for (w = 0, k = 0; k < bytes / 4; k++)
                    if (((uint32_t *)(blob + off))[k] == GD_RT_PA >> 3) {
                        h->diag_patch = off + k * 4;
                        w++;
                    }
                if (w != 1) {
                    fprintf(stderr, "genmirror: colour buffer address found %u times\n", w);
                    return 1;
                }
            }
            off = (off + bytes + 15) & ~15u;
        }
    }
    h->magic = GM_BLOB_MAGIC;
    h->version = GM_BLOB_VERSION;
    h->blob_pa = GM_BLOB_PA;
    h->size = off;
    h->scr_pa = GM_SCR_PA;
    h->rt_top_pa = GM_RT_TOP_PA;
    h->rt_bot_pa = GM_RT_BOT_PA;
    h->src_wh = GM_SRC_H << 16 | GM_SRC_W;
    if (off > GM_BLOB_MAX) {
        fprintf(stderr, "genmirror: blob is %u bytes, limit %u\n", off, GM_BLOB_MAX);
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (!f || fwrite(blob, 1, off, f) != off || fclose(f)) {
        perror(argv[1]);
        return 1;
    }
    printf("genmirror: %s: %u bytes (flat %u + %u, stereo %u + %u bytes) for PA 0x%08x\n",
           argv[1], off, h->list_bytes[0], h->list_bytes[1], h->st_list_bytes[0], h->st_list_bytes[1], GM_BLOB_PA);
    return 0;
}
