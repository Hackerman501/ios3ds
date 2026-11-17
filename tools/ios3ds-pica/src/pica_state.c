/*
 * ios3ds-pica: register-level state encoders. Each function appends the
 * command words for one piece of pipeline state; none of them keeps state.
 * The write sequences follow what citro3d sends for the same state (the
 * order matters for a few registers), see docs/pica200.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "pica.h"

/* ---- render target ---- */

uint32_t pica_fb_color_bpp(unsigned fmt)
{
    static const uint8_t bpp[] = { 4, 3, 2, 2, 2 };
    return fmt < 5 ? bpp[fmt] : 0;
}

uint32_t pica_fb_depth_bpp(unsigned fmt)
{
    static const uint8_t bpp[] = { 2, 0, 3, 4 };
    return fmt < 4 ? bpp[fmt] : 0;
}

void pica_emit_fb_flush(pica_cmd_t *c)
{
    pica_cmd_write(c, P3D_FRAMEBUFFER_FLUSH, 1);
    pica_cmd_write(c, P3D_FRAMEBUFFER_INVALIDATE, 1);
    pica_cmd_write(c, P3D_EARLYDEPTH_CLEAR, 1);
}

void pica_emit_framebuffer(pica_cmd_t *c, const pica_fb_t *fb)
{
    uint32_t v[4];
    uint32_t dim = 0x01000000u | (((uint32_t)fb->height - 1) & 0xFFF) << 12 | (fb->width & 0xFFF);

    pica_cmd_write(c, P3D_FRAMEBUFFER_INVALIDATE, 1);
    v[0] = fb->depth_pa >> 3;
    v[1] = fb->color_pa >> 3;
    v[2] = dim;
    pica_cmd_write_seq(c, P3D_DEPTHBUFFER_LOC, v, 3);
    pica_cmd_write(c, P3D_RENDERBUF_DIM, dim);
    pica_cmd_write(c, P3D_DEPTHBUFFER_FORMAT, fb->depth_fmt);
    /* low bits: pixel size code (bytes - 2), high: format */
    pica_cmd_write(c, P3D_COLORBUFFER_FORMAT,
                   (pica_fb_color_bpp(fb->color_fmt) - 2) | (uint32_t)fb->color_fmt << 16);
    pica_cmd_write(c, P3D_FRAMEBUFFER_BLOCK32, 0);
    /* buffer access enables: colour read, colour write, depth read, depth write */
    v[0] = v[1] = fb->color_pa ? 0xF : 0;
    v[2] = v[3] = !fb->depth_pa ? 0 : fb->depth_fmt == PICA_DEPTH24S8 ? 3 : 2;
    pica_cmd_write_seq(c, P3D_COLORBUFFER_READ, v, 4);
}

void pica_emit_viewport(pica_cmd_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t v[4];

    v[0] = pica_f24((float)w / 2.0f);
    v[1] = pica_f31(2.0f / (float)w) << 1;
    v[2] = pica_f24((float)h / 2.0f);
    v[3] = pica_f31(2.0f / (float)h) << 1;
    pica_cmd_write_seq(c, P3D_VIEWPORT_WIDTH, v, 4);
    pica_cmd_write(c, P3D_VIEWPORT_XY, y << 16 | (x & 0xFFFF));
}

void pica_emit_scissor(pica_cmd_t *c, int enable, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t v[3] = { PICA_SCISSOR_OFF, 0, 0 };

    if (enable && w && h) {
        v[0] = PICA_SCISSOR_ON;
        v[1] = y << 16 | (x & 0xFFFF);
        v[2] = (y + h - 1) << 16 | ((x + w - 1) & 0xFFFF);
    } else if (enable) {
        /* empty box: nothing may be drawn = "only outside the largest possible box" */
        v[0] = PICA_SCISSOR_INVERT;
        v[1] = 0;
        v[2] = 0x03FF03FF;
    }
    pica_cmd_write_seq(c, P3D_SCISSORTEST_MODE, v, 3);
}

/* ---- per-fragment state ---- */

void pica_raster_defaults(pica_raster_t *r)
{
    memset(r, 0, sizeof *r);
    r->cull = PICA_CULL_NONE;
    r->depth_func = PICA_LESS;
    r->write_mask = PICA_WRITE_ALL;
    r->alpha_func = PICA_ALWAYS;
    r->stencil_func = PICA_ALWAYS;
    r->stencil_mask = 0xFF;
    r->stencil_write_mask = 0xFF;
    r->blend_src_rgb = r->blend_src_a = PICA_BF_ONE;
    r->blend_dst_rgb = r->blend_dst_a = PICA_BF_ZERO;
    r->logic_op = 3;                    /* copy */
    r->depth_near = 0.0f;
    r->depth_far = 1.0f;
}

void pica_emit_raster(pica_cmd_t *c, const pica_raster_t *r)
{
    uint32_t v[4];
    uint32_t wmask = r->write_mask;

    /* The GPU writes depth whenever the write bit is set, tested or not;
       GL only writes depth while the depth test is enabled. */
    if (!r->depth_test)
        wmask &= ~(uint32_t)PICA_WRITE_DEPTH;

    pica_cmd_write(c, P3D_DEPTHMAP_ENABLE, 1);          /* z-buffer, not w-buffer */
    pica_cmd_write(c, P3D_FACECULLING_CONFIG, r->cull & 3);
    /* clip z/w is in [-1 (near), 0 (far)]:  depth = z/w * scale + offset */
    v[0] = pica_f24(r->depth_far - r->depth_near);
    v[1] = pica_f24(r->depth_far + r->depth_bias);
    pica_cmd_write_seq(c, P3D_DEPTHMAP_SCALE, v, 2);

    v[0] = (r->alpha_test ? 1u : 0u) | (r->alpha_func & 7u) << 4 | (uint32_t)r->alpha_ref << 8;
    v[1] = (r->stencil_test ? 1u : 0u) | (r->stencil_func & 7u) << 4 |
           (uint32_t)r->stencil_write_mask << 8 | (uint32_t)r->stencil_ref << 16 |
           (uint32_t)r->stencil_mask << 24;
    v[2] = (r->stencil_sfail & 7u) | (r->stencil_zfail & 7u) << 4 | (r->stencil_zpass & 7u) << 8;
    v[3] = (r->depth_test ? 1u : 0u) | (r->depth_func & 7u) << 4 | (wmask & 0x1F) << 8;
    pica_cmd_write_seq(c, P3D_FRAGOP_ALPHA_TEST, v, 4);
    /* gas depth function must mirror the depth function (citro3d does the same) */
    pica_cmd_write_masked(c, P3D_GAS_DELTAZ_DEPTH, 0x8, ((0xAF02u >> ((r->depth_func & 7) << 1)) & 3) << 24);

    pica_cmd_write(c, P3D_BLEND_COLOR, r->blend_color);
    if (r->blend && !r->logic_op_enable)
        pica_cmd_write(c, P3D_BLEND_FUNC,
                       (r->blend_eq_rgb & 7u) | (r->blend_eq_a & 7u) << 8 |
                       (r->blend_src_rgb & 15u) << 16 | (r->blend_dst_rgb & 15u) << 20 |
                       (r->blend_src_a & 15u) << 24 | (uint32_t)(r->blend_dst_a & 15u) << 28);
    else    /* there is no "blending off": source * 1 + destination * 0 */
        pica_cmd_write(c, P3D_BLEND_FUNC, (uint32_t)PICA_BF_ONE << 16 | (uint32_t)PICA_BF_ONE << 24);
    pica_cmd_write(c, P3D_LOGIC_OP, r->logic_op & 15u);
    /* byte 0: fragment operation mode (0 = normal); byte 1: 1 = blend, 0 = logic op;
       byte 2: 0xE4, the value every known user writes */
    pica_cmd_write_masked(c, P3D_COLOR_OPERATION, 0x7, 0x00E40000u | (r->logic_op_enable ? 0 : 0x100));
}

/* ---- textures ---- */

void pica_emit_texture(pica_cmd_t *c, unsigned unit, const pica_tex_t *t)
{
    static const uint16_t base[3] = { P3D_TEXUNIT0_BORDER_COLOR, P3D_TEXUNIT1_BORDER_COLOR, P3D_TEXUNIT2_BORDER_COLOR };
    static const uint16_t type[3] = { P3D_TEXUNIT0_TYPE, P3D_TEXUNIT1_TYPE, P3D_TEXUNIT2_TYPE };
    uint32_t v[5];

    if (unit > 2)
        return;
    v[0] = t->border;
    v[1] = (uint32_t)t->width << 16 | t->height;
    v[2] = (t->mag & 1u) << 1 | (t->min & 1u) << 2 | (t->wrap_t & 3u) << 8 | (t->wrap_s & 3u) << 12;
    v[3] = 0;                           /* LOD: one level, no bias */
    v[4] = t->pa >> 3;
    pica_cmd_write_seq(c, base[unit], v, 5);
    pica_cmd_write(c, type[unit], t->fmt);
}

void pica_emit_texunits(pica_cmd_t *c, unsigned unit_mask)
{
    /* bit 12 is set by every known user; bit 16 = clear the texture cache,
       which only takes effect when written after the enables */
    pica_cmd_write_masked(c, P3D_TEXUNIT_CONFIG, 0xB, (unit_mask & 7u) | 1u << 12);
    pica_cmd_write_masked(c, P3D_TEXUNIT_CONFIG, 0x4, 1u << 16);
    pica_cmd_write(c, P3D_TEXUNIT0_SHADOW, 1);
}

void pica_tev_passthrough(pica_tev_t *t)
{
    memset(t, 0, sizeof *t);
    t->src_rgb[0] = t->src_a[0] = PICA_TEVSRC_PREVIOUS;
    t->func_rgb = t->func_a = PICA_TEV_REPLACE;
    t->color = 0xFFFFFFFFu;
}

void pica_emit_tev(pica_cmd_t *c, unsigned stage, const pica_tev_t *t)
{
    static const uint16_t base[PICA_TEV_STAGES] = {
        P3D_TEXENV0_SOURCE, P3D_TEXENV1_SOURCE, P3D_TEXENV2_SOURCE,
        P3D_TEXENV3_SOURCE, P3D_TEXENV4_SOURCE, P3D_TEXENV5_SOURCE };
    uint32_t v[5];

    if (stage >= PICA_TEV_STAGES)
        return;
    v[0] = (t->src_rgb[0] & 15u) | (t->src_rgb[1] & 15u) << 4 | (t->src_rgb[2] & 15u) << 8 |
           (t->src_a[0] & 15u) << 16 | (t->src_a[1] & 15u) << 20 | (t->src_a[2] & 15u) << 24;
    v[1] = (t->op_rgb[0] & 15u) | (t->op_rgb[1] & 15u) << 4 | (t->op_rgb[2] & 15u) << 8 |
           (t->op_a[0] & 7u) << 12 | (t->op_a[1] & 7u) << 16 | (t->op_a[2] & 7u) << 20;
    v[2] = (t->func_rgb & 15u) | (t->func_a & 15u) << 16;
    v[3] = t->color;
    v[4] = (t->scale_rgb & 3u) | (t->scale_a & 3u) << 16;
    pica_cmd_write_seq(c, base[stage], v, 5);
}

/* ---- vertex input ---- */

int pica_emit_vtx_layout(pica_cmd_t *c, const pica_vtx_layout_t *l)
{
    static const uint8_t tsize[4] = { 1, 1, 2, 4 };
    uint32_t fmt[2] = { 0, 0 };
    uint32_t perm[2] = { 0, 0 };
    uint32_t bufs[36];
    unsigned i, b;

    if (l->count == 0 || l->count > 12 || l->nbuffers > 12 || (l->base_pa & 7))
        return -1;

    /* formats: 4 bits per attribute (type | (size-1) << 2), 12 attributes
       across the two words; then the fixed-attribute mask and the count */
    for (i = 0; i < l->count; i++) {
        uint32_t f = (l->attr[i].type & 3u) | ((l->attr[i].size - 1u) & 3u) << 2;
        if (l->attr[i].size < 1 || l->attr[i].size > 4)
            return -1;
        if (i < 8)
            fmt[0] |= f << (i * 4);
        else
            fmt[1] |= f << ((i - 8) * 4);
        if (l->attr[i].fixed)
            fmt[1] |= 1u << (16 + i);
        /* attribute i -> shader input register v<i> */
        if (i < 8)
            perm[0] |= i << (i * 4);
        else
            perm[1] |= i << ((i - 8) * 4);
    }
    fmt[1] |= (uint32_t)(l->count - 1) << 28;

    /* buffers: which attributes each one holds, in storage order. The GPU
       aligns every attribute to its component size; a gap up to the stride
       is described with padding entries (12..15 = 4, 8, 12, 16 bytes). */
    memset(bufs, 0, sizeof bufs);
    for (b = 0; b < l->nbuffers; b++) {
        uint64_t comps = 0;
        unsigned n = 0, off = 0;

        for (i = 0; i < l->count; i++) {
            unsigned ts;
            if (l->attr[i].fixed || l->attr[i].buffer != b)
                continue;
            ts = tsize[l->attr[i].type & 3];
            off = (off + ts - 1) & ~(ts - 1);
            off += ts * l->attr[i].size;
            comps |= (uint64_t)i << (n++ * 4);
        }
        if (n == 0)
            continue;
        if (off > l->buffer[b].stride || ((l->buffer[b].stride - off) & 3))
            return -1;
        for (unsigned pad = l->buffer[b].stride - off; pad; ) {
            unsigned k = pad > 16 ? 16 : pad;
            if (n == 12)
                return -1;
            comps |= (uint64_t)(11 + k / 4) << (n++ * 4);
            pad -= k;
        }
        bufs[b * 3 + 0] = l->buffer[b].offset;
        bufs[b * 3 + 1] = (uint32_t)comps;
        bufs[b * 3 + 2] = (uint32_t)(comps >> 32) | (uint32_t)l->buffer[b].stride << 16 | (uint32_t)n << 28;
    }

    pica_cmd_write_seq(c, P3D_ATTRIBBUFFERS_FORMAT_LOW, fmt, 2);
    pica_cmd_write_masked(c, P3D_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000u | (l->count - 1u));
    pica_cmd_write(c, P3D_VSH_NUM_ATTR, l->count - 1u);
    pica_cmd_write_seq(c, P3D_VSH_ATTR_PERM_LOW, perm, 2);
    pica_cmd_write(c, P3D_ATTRIBBUFFERS_LOC, l->base_pa >> 3);
    pica_cmd_write_seq(c, P3D_ATTRIBBUFFER0_OFFSET, bufs, 36);
    return 0;
}

void pica_emit_fixed_attr(pica_cmd_t *c, unsigned index, const float f[4])
{
    uint32_t x = pica_f24(f[0]), y = pica_f24(f[1]), z = pica_f24(f[2]), w = pica_f24(f[3]);
    uint32_t v[3];

    /* four 24-bit floats packed into 96 bits, most significant word first */
    v[0] = w << 8 | z >> 16;
    v[1] = z << 16 | y >> 8;
    v[2] = y << 24 | x;
    pica_cmd_write(c, P3D_FIXEDATTRIB_INDEX, index);
    pica_cmd_write_seq(c, P3D_FIXEDATTRIB_DATA0, v, 3);
}

/* ---- draws ---- */

void pica_emit_draw_arrays(pica_cmd_t *c, unsigned prim, uint32_t first, uint32_t count)
{
    pica_cmd_write_masked(c, P3D_PRIMITIVE_CONFIG, 2, prim);
    pica_cmd_write(c, P3D_RESTART_PRIMITIVE, 1);
    pica_cmd_write(c, P3D_INDEXBUFFER_CONFIG, 0x80000000u);     /* unused, but must be written */
    pica_cmd_write(c, P3D_NUMVERTICES, count);
    pica_cmd_write(c, P3D_VERTEX_OFFSET, first);
    pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG2, 1, 1);       /* array drawing */
    pica_cmd_write_masked(c, P3D_START_DRAW_FUNC0, 1, 0);       /* draw mode */
    pica_cmd_write(c, P3D_DRAWARRAYS, 1);
    pica_cmd_write_masked(c, P3D_START_DRAW_FUNC0, 1, 1);       /* back to configuration mode */
    pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG2, 1, 0);
    pica_cmd_write(c, P3D_VTX_FUNC, 1);                         /* clear the post-vertex cache */
}

void pica_emit_draw_elements(pica_cmd_t *c, unsigned prim, uint32_t count, uint32_t index_off, int is16)
{
    int list = prim == PICA_TRIANGLES;

    /* indexed triangle lists go through the "geometry primitive" path */
    pica_cmd_write_masked(c, P3D_PRIMITIVE_CONFIG, 2, list ? PICA_GEOMETRY_PRIM : prim);
    pica_cmd_write(c, P3D_RESTART_PRIMITIVE, 1);
    pica_cmd_write(c, P3D_INDEXBUFFER_CONFIG, (index_off & 0x0FFFFFFFu) | (is16 ? 0x80000000u : 0));
    pica_cmd_write(c, P3D_NUMVERTICES, count);
    pica_cmd_write(c, P3D_VERTEX_OFFSET, 0);
    if (list) {
        pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG, 2, 0x100);
        pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG2, 2, 0x100);
    }
    pica_cmd_write_masked(c, P3D_START_DRAW_FUNC0, 1, 0);
    pica_cmd_write(c, P3D_DRAWELEMENTS, 1);
    pica_cmd_write_masked(c, P3D_START_DRAW_FUNC0, 1, 1);
    if (list) {
        pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG, 2, 0);
        pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG2, 2, 0);
    }
    pica_cmd_write(c, P3D_VTX_FUNC, 1);
    pica_cmd_write_masked(c, P3D_PRIMITIVE_CONFIG, 0x8, 0);
    pica_cmd_write_masked(c, P3D_PRIMITIVE_CONFIG, 0x8, 0);
}

/* ---- shader ---- */

void pica_emit_shader(pica_cmd_t *c, const pica_shader_t *s)
{
    uint32_t v[8];
    static const uint32_t zero[4];

    /* geometry stage off (set early so all four shader units are in a known state) */
    pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG, 0x3, 0);
    pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG2, 0x3, 0);
    pica_cmd_write_masked(c, P3D_VSH_COM_MODE, 0x1, 0);

    pica_cmd_write(c, P3D_VSH_CODETRANSFER_CONFIG, 0);
    pica_cmd_write_burst(c, P3D_VSH_CODETRANSFER_DATA, s->code, s->ncode);
    pica_cmd_write(c, P3D_VSH_CODETRANSFER_END, 1);
    pica_cmd_write(c, P3D_VSH_OPDESCS_CONFIG, 0);
    pica_cmd_write_burst(c, P3D_VSH_OPDESCS_DATA, s->opdesc, s->nopdesc);

    pica_cmd_write(c, P3D_VSH_ENTRYPOINT, 0x7FFF0000u | (s->entry & 0xFFFF));
    pica_cmd_write(c, P3D_VSH_OUTMAP_MASK, s->outmap_mask);
    pica_cmd_write(c, P3D_VSH_OUTMAP_TOTAL1, s->outmap_total - 1);
    pica_cmd_write(c, P3D_VSH_OUTMAP_TOTAL2, s->outmap_total - 1);

    /* which shader output component carries which rasterizer input */
    pica_cmd_write_masked(c, P3D_PRIMITIVE_CONFIG, 0x1, s->outmap_total - 1);
    v[0] = s->outmap_total;
    memcpy(v + 1, s->outmap, sizeof s->outmap);
    pica_cmd_write_seq(c, P3D_SH_OUTMAP_TOTAL, v, 8);
    pica_cmd_write(c, P3D_SH_OUTATTR_MODE, s->outattr_mode);
    pica_cmd_write(c, P3D_SH_OUTATTR_CLOCK, s->outattr_clock);

    pica_cmd_write_masked(c, P3D_GEOSTAGE_CONFIG, 0xA, 0);
    pica_cmd_write(c, P3D_GSH_MISC0, 0);
    pica_cmd_write(c, P3D_GSH_MISC1, 0);
    pica_cmd_write(c, P3D_GSH_INPUTBUFFER_CONFIG, 0xA0000000u);

    pica_cmd_write(c, P3D_VSH_BOOLUNIFORM, 0x7FFF0000u);
    pica_cmd_write_seq(c, P3D_VSH_INTUNIFORM_I0, zero, 4);
}

void pica_emit_uniforms(pica_cmd_t *c, unsigned first, const float (*f)[4], unsigned n)
{
    uint32_t v[4];

    /* bit 31: the data that follows is IEEE single floats, 4 words per
       vector, w first. The index auto-increments. */
    pica_cmd_write(c, P3D_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | first);
    for (unsigned i = 0; i < n; i++) {
        memcpy(&v[0], &f[i][3], 4);
        memcpy(&v[1], &f[i][2], 4);
        memcpy(&v[2], &f[i][1], 4);
        memcpy(&v[3], &f[i][0], 4);
        pica_cmd_write_burst(c, P3D_VSH_FLOATUNIFORM_DATA, v, 4);
    }
}

void pica_emit_base_state(pica_cmd_t *c)
{
    pica_tev_t t;

    pica_cmd_write(c, P3D_LIGHTING_ENABLE0, 0);
    pica_cmd_write(c, P3D_LIGHTING_ENABLE1, 1);
    pica_cmd_write_masked(c, P3D_TEXENV_UPDATE_BUFFER, 0x7, 0);     /* no fog, no combiner buffer */
    pica_cmd_write(c, P3D_TEXENV_BUFFER_COLOR, 0xFFFFFFFFu);
    pica_cmd_write(c, P3D_FOG_COLOR, 0);
    pica_cmd_write_masked(c, P3D_EARLYDEPTH_TEST1, 1, 0);
    pica_cmd_write(c, P3D_EARLYDEPTH_TEST2, 0);

    /* Stage 0 names the vertex colour explicitly: what "previous" means in
       the first stage is not something to rely on. */
    pica_tev_passthrough(&t);
    t.src_rgb[0] = t.src_a[0] = PICA_TEVSRC_PRIMARY;
    pica_emit_tev(c, 0, &t);
    pica_tev_passthrough(&t);
    for (unsigned i = 1; i < PICA_TEV_STAGES; i++)
        pica_emit_tev(c, i, &t);
    pica_emit_texunits(c, 0);
}
