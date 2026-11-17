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
 * allocated from the FCRAM arena) is patched in by the shim (tex_patch).
 *
 * Two lists, identical except for their vertex arrays:
 *   [0] texture coordinates for a GPU whose nearest sampling truncates
 *       (texel = floor(s * width), what Citra/Azahar and our model do)
 *   [1] the same shifted by -1/2 texel, for one that rounds (GM_VAR_SAMPLE_HALF)
 * The shim's self-test picks the one that reproduces the CPU copy exactly.
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
           iOS row y is stored row y of the 512x512 texture, so t * 512 = 512 - Y */
        v[i].t0[0] = (X + shift) / (float)GM_TEX_W;
        v[i].t0[1] = ((float)GM_TEX_H - Y + shift) / (float)GM_TEX_H;
        v[i].fog = 1.0f;
    }
}

/* what one list draws: a render target, an optional texture, a TEV program,
   and a run of triangles from the list's vertex array */
typedef struct {
    uint32_t rt_pa, rt_w, rt_h;
    uint32_t tex_pa, tex_w, tex_h;      /* tex_pa 0 = untextured */
    int tev;                            /* 0 mirror swizzle, 1 constant colour, 2 replace texture */
    uint32_t first, count;
} Pass;

static void emit_prologue(pica_cmd_t *c, uint32_t vtx_pa, const Pass *p)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    pica_tex_t t;
    pica_tev_t tev[PICA_TEV_STAGES];
    pica_raster_t rs;
    pica_vtx_layout_t l;
    float clip[16];
    int i;

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
        pica_emit_texunits(c, 1);
    } else {
        pica_emit_texunits(c, 0);
    }

    for (i = 0; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&tev[i]);
    if (p->tev == 0) {
        /* The texture holds the iOS bytes B,G,R,A, read as RGBA8 (bytes A,B,G,R):
           sample.a = B, .b = G, .g = R, .r = A. Rebuild the real colour:
             stage 0: (sample.g) * (1,0,0)            -> (R, 0, 0)
             stage 1: (sample.b) * (0,1,0) + previous -> (R, G, 0)
             stage 2: (sample.a) * (0,0,1) + previous -> (R, G, B)
           Multiplying by 0 or 255 and adding disjoint channels is exact. */
        for (i = 0; i < 3; i++) {
            static const uint8_t op[3] = { PICA_TEVOP_RGB_G, PICA_TEVOP_RGB_B, PICA_TEVOP_RGB_ALPHA };
            static const uint32_t k[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u };     /* R | G<<8 | B<<16 | A<<24 */
            tev[i].src_rgb[0] = PICA_TEVSRC_TEXTURE0;
            tev[i].src_rgb[1] = PICA_TEVSRC_CONSTANT;
            tev[i].src_rgb[2] = i ? PICA_TEVSRC_PREVIOUS : PICA_TEVSRC_CONSTANT;
            tev[i].op_rgb[0] = op[i];
            tev[i].op_rgb[1] = PICA_TEVOP_RGB_COLOR;
            tev[i].op_rgb[2] = PICA_TEVOP_RGB_COLOR;
            tev[i].func_rgb = i ? PICA_TEV_MULT_ADD : PICA_TEV_MODULATE;
            tev[i].src_a[0] = PICA_TEVSRC_CONSTANT;
            tev[i].op_a[0] = PICA_TEVOP_A_ALPHA;
            tev[i].func_a = PICA_TEV_REPLACE;
            tev[i].color = k[i];
        }
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
    emit_prologue(&c, vtx_pa, &passes[0]);
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

int main(int argc, char **argv)
{
    static uint8_t blob[GM_BLOB_MAX];
    gm_blob_t *h = (gm_blob_t *)blob;
    uint32_t off = (sizeof *h + 15) & ~15u, vtx_off[2], i;

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
            { GM_RT_TOP_PA, GM_RT_W, GM_RT_TOP_H, GM_TEX_PLACEHOLDER, GM_TEX_W, GM_TEX_H, 0, 0, 6 },
            { GM_RT_BOT_PA, GM_RT_W, GM_RT_BOT_H, GM_TEX_PLACEHOLDER, GM_TEX_W, GM_TEX_H, 0, 6, 6 },
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
    /* diagnostics (pica_diag.c): one solid quad, one 64x64 and one 48x40
       texture drawn 1:1, all into a 64x64 VRAM target */
    {
        MVtx v[12];
        uint32_t dv = off, k, w;
        gl_quad(v, GD_RT_W, GD_RT_W, 0, GD_RT_W, 0, GD_RT_W);
        gl_quad(v + 6, GD_RT_W, GD_RT_W, 0, GD_NPOT_W, 0, GD_NPOT_H);
        memcpy(blob + off, v, sizeof v);
        off = (off + sizeof v + 15) & ~15u;
        for (i = 0; i < 3; i++) {
            const Pass p = { GD_RT_PA, GD_RT_W, GD_RT_W, i ? GD_TEX_PA : 0, i == 2 ? GD_NPOT_W : GD_RT_W,
                             i == 2 ? GD_NPOT_H : GD_RT_W, i ? 2 : 1, i == 2 ? 6 : 0, 6 };
            uint32_t bytes = build_list((uint32_t *)(blob + off), (GM_BLOB_MAX - off) / 4, GM_BLOB_PA + dv, &p, 1);
            if (!bytes) {
                fprintf(stderr, "genmirror: diagnostic list %u does not fit\n", i);
                return 1;
            }
            h->diag_off[i] = off;
            h->diag_bytes[i] = bytes;
            if (i == 0) {               /* the colour buffer address word, for rendering into FCRAM */
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
    f = fopen(argv[1], "wb");
    if (!f || fwrite(blob, 1, off, f) != off || fclose(f)) {
        perror(argv[1]);
        return 1;
    }
    printf("genmirror: %s: %u bytes (lists %u + %u bytes) for PA 0x%08x\n", argv[1], off,
           h->list_bytes[0], h->list_bytes[1], GM_BLOB_PA);
    return 0;
}
