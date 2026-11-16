/*
 * ios3ds-pica: OpenGL ES 1.1 fixed function on a GPU that has none.
 *   - vertex transform: the shader in shader/ff.vsh plus its uniforms
 *   - glTexEnv: TEV combiner stages
 *   - GL enum -> register field translation
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "pica.h"
#include "pica_ff_shader.h"

/* GL constants used here (values from the Khronos GLES 1.1 headers) */
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_ADD 0x0104
#define GL_NEVER 0x0200
#define GL_SRC_COLOR 0x0300
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_BLEND 0x0BE2
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_FLOAT 0x1406
#define GL_CLEAR 0x1500
#define GL_INVERT 0x150A
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_INCR 0x1E02
#define GL_DECR 0x1E03
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_REPEAT 0x2901
#define GL_FUNC_ADD 0x8006
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_BGRA 0x80E1
#define GL_TEXTURE 0x1702
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COMBINE 0x8570
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SUBTRACT 0x84E7
#define GL_DOT3_RGB 0x86AE
#define GL_DOT3_RGBA 0x86AF
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508

const pica_shader_t pica_ff_shader = {
    .code = pica_ff_code,
    .ncode = sizeof pica_ff_code / 4,
    .opdesc = pica_ff_opdesc,
    .nopdesc = sizeof pica_ff_opdesc / 4,
    .entry = PICA_FF_ENTRY,
    .outmap_total = PICA_FF_OUTMAP_TOTAL,
    .outmap_mask = PICA_FF_OUTMAP_MASK,
    .outmap = PICA_FF_OUTMAP,
    .outattr_mode = PICA_FF_OUTATTR_MODE,
    .outattr_clock = PICA_FF_OUTATTR_CLOCK,
};

void pica_ff_emit_program(pica_cmd_t *c)
{
    static const float c9[1][4] = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    pica_emit_shader(c, &pica_ff_shader);
    pica_emit_uniforms(c, 9, c9, 1);
}

/* out = a * b, GL column-major */
static void mul44(float out[16], const float a[16], const float b[16])
{
    float t[16];

    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            t[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
    memcpy(out, t, sizeof t);
}

void pica_ff_clip_matrix(float out[16], const float projection[16], const float modelview[16])
{
    mul44(out, projection, modelview);
    /* GL keeps -w <= z <= w (near .. far); the GPU keeps -w <= z <= 0.
       z' = (z - w) / 2 maps one onto the other, near staying nearest. */
    for (int col = 0; col < 4; col++)
        out[col * 4 + 2] = 0.5f * (out[col * 4 + 2] - out[col * 4 + 3]);
}

void pica_ff_tex_matrix(float out[16], const float m[16], uint32_t w, uint32_t h, uint32_t tw, uint32_t th)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float sx = (float)w / (float)tw, sy = (float)h / (float)th;

    memcpy(out, m ? m : ident, sizeof ident);
    for (int col = 0; col < 4; col++) {
        out[col * 4 + 0] *= sx;
        out[col * 4 + 1] *= sy;
    }
}

static void row(float dst[4], const float m[16], int r)
{
    dst[0] = m[0 + r]; dst[1] = m[4 + r]; dst[2] = m[8 + r]; dst[3] = m[12 + r];
}

void pica_ff_emit_matrices(pica_cmd_t *c, const float clip[16],
                           const float tex0[16], const float tex1[16], float color_scale)
{
    float u[9][4];

    for (int r = 0; r < 4; r++)
        row(u[r], clip, r);
    for (int i = 0; i < 2; i++) {
        const float *t = i ? tex1 : tex0;
        if (t) {
            row(u[4 + i * 2], t, 0);
            row(u[5 + i * 2], t, 1);
        } else {
            static const float s[4] = { 1, 0, 0, 0 }, tt[4] = { 0, 1, 0, 0 };
            memcpy(u[4 + i * 2], s, sizeof s);
            memcpy(u[5 + i * 2], tt, sizeof tt);
        }
    }
    u[8][0] = u[8][1] = u[8][2] = u[8][3] = color_scale;
    pica_emit_uniforms(c, 0, (const float (*)[4])u, 9);
}

/* ---- glTexEnv ---- */

/*
 * GLES 1.1 table 3.15/3.16, one combiner stage per texture unit. Cp/Ap is
 * the incoming colour (vertex colour for the first unit), Cs/As the texture
 * sample, Cc the env colour. The GPU samples textures without the channel
 * as (L, L, L) / alpha 1 / colour 0, so "keep the incoming alpha" and "keep
 * the incoming colour" are spelled out instead of relying on the sample.
 */
static int one_unit(pica_tev_t *t, const pica_gl_texenv_t *e, unsigned texsrc, unsigned prev)
{
    int has_rgb = e->base_format != GL_ALPHA;
    int has_a = e->base_format == GL_RGBA || e->base_format == GL_BGRA ||
                e->base_format == GL_LUMINANCE_ALPHA || e->base_format == GL_ALPHA;

    pica_tev_passthrough(t);
    t->color = e->env_color;
    t->src_rgb[0] = t->src_a[0] = (uint8_t)prev;        /* default: C = Cp, A = Ap */

    /* alpha: REPLACE takes As, DECAL keeps Ap, the rest multiply */
    if (has_a && e->mode != GL_DECAL) {
        t->src_a[0] = (uint8_t)texsrc;
        if (e->mode != GL_REPLACE) {
            t->src_a[1] = (uint8_t)prev;
            t->func_a = PICA_TEV_MODULATE;
        }
    }
    if (!has_rgb)
        return 0;                                       /* GL_ALPHA textures: C = Cp */

    switch (e->mode) {
    case GL_REPLACE:
        t->src_rgb[0] = (uint8_t)texsrc;
        break;
    case GL_MODULATE:
        t->src_rgb[0] = (uint8_t)texsrc;
        t->src_rgb[1] = (uint8_t)prev;
        t->func_rgb = PICA_TEV_MODULATE;
        break;
    case GL_ADD:
        t->src_rgb[0] = (uint8_t)texsrc;
        t->src_rgb[1] = (uint8_t)prev;
        t->func_rgb = PICA_TEV_ADD;
        break;
    case GL_DECAL:
        if (!has_a) {                                   /* RGB: C = Cs */
            t->src_rgb[0] = (uint8_t)texsrc;
            break;
        }
        /* RGBA: C = Cs * As + Cp * (1 - As) */
        t->src_rgb[0] = (uint8_t)texsrc;
        t->src_rgb[1] = (uint8_t)prev;
        t->src_rgb[2] = (uint8_t)texsrc;
        t->op_rgb[2] = PICA_TEVOP_RGB_ALPHA;
        t->func_rgb = PICA_TEV_INTERPOLATE;
        break;
    case GL_BLEND:
        /* C = Cc * Cs + Cp * (1 - Cs) */
        t->src_rgb[0] = PICA_TEVSRC_CONSTANT;
        t->src_rgb[1] = (uint8_t)prev;
        t->src_rgb[2] = (uint8_t)texsrc;
        t->func_rgb = PICA_TEV_INTERPOLATE;
        break;
    default:
        return -1;
    }
    return 0;
}

/* GL_COMBINE: sources, operands and functions map one to one. */
static int combine_unit(pica_tev_t *t, const pica_gl_texenv_t *e, unsigned texsrc, unsigned prev)
{
    int i;
    pica_tev_passthrough(t);
    t->color = e->env_color;
    for (i = 0; i < 3; i++) {
        uint32_t s[2] = { e->src_rgb[i], e->src_a[i] };
        uint8_t out[2];
        for (int k = 0; k < 2; k++)
            switch (s[k]) {
            case GL_TEXTURE: out[k] = (uint8_t)texsrc; break;
            case GL_CONSTANT: out[k] = PICA_TEVSRC_CONSTANT; break;
            case GL_PRIMARY_COLOR: out[k] = PICA_TEVSRC_PRIMARY; break;
            case GL_PREVIOUS: out[k] = (uint8_t)prev; break;
            default: return -1;
            }
        t->src_rgb[i] = out[0];
        t->src_a[i] = out[1];
        switch (e->op_rgb[i]) {
        case GL_SRC_COLOR: t->op_rgb[i] = PICA_TEVOP_RGB_COLOR; break;
        case GL_ONE_MINUS_SRC_COLOR: t->op_rgb[i] = PICA_TEVOP_RGB_INV_COLOR; break;
        case GL_SRC_ALPHA: t->op_rgb[i] = PICA_TEVOP_RGB_ALPHA; break;
        case GL_ONE_MINUS_SRC_ALPHA: t->op_rgb[i] = PICA_TEVOP_RGB_INV_ALPHA; break;
        default: return -1;
        }
        t->op_a[i] = e->op_a[i] == GL_ONE_MINUS_SRC_ALPHA ? PICA_TEVOP_A_INV_ALPHA : PICA_TEVOP_A_ALPHA;
    }
    for (i = 0; i < 2; i++) {
        uint8_t f;
        switch (i ? e->comb_a : e->comb_rgb) {
        case GL_REPLACE: f = PICA_TEV_REPLACE; break;
        case GL_MODULATE: f = PICA_TEV_MODULATE; break;
        case GL_ADD: f = PICA_TEV_ADD; break;
        case GL_ADD_SIGNED: f = PICA_TEV_ADD_SIGNED; break;
        case GL_INTERPOLATE: f = PICA_TEV_INTERPOLATE; break;
        case GL_SUBTRACT: f = PICA_TEV_SUBTRACT; break;
        case GL_DOT3_RGB: f = PICA_TEV_DOT3_RGB; break;
        case GL_DOT3_RGBA: f = PICA_TEV_DOT3_RGBA; break;
        default: return -1;
        }
        if (i) t->func_a = f; else t->func_rgb = f;
    }
    t->scale_rgb = e->scale_rgb == 4 ? 2 : e->scale_rgb == 2 ? 1 : 0;
    t->scale_a = e->scale_a == 4 ? 2 : e->scale_a == 2 ? 1 : 0;
    return 0;
}

int pica_ff_texenv(pica_tev_t out[PICA_TEV_STAGES], const pica_gl_texenv_t *units[2])
{
    int n = 0;

    for (int i = 0; i < 2; i++) {
        if (!units[i])
            continue;
        unsigned prev = n ? PICA_TEVSRC_PREVIOUS : PICA_TEVSRC_PRIMARY;
        if (units[i]->mode == GL_COMBINE ? combine_unit(&out[n], units[i], PICA_TEVSRC_TEXTURE0 + i, prev)
                                         : one_unit(&out[n], units[i], PICA_TEVSRC_TEXTURE0 + i, prev))
            return -1;
        n++;
    }
    if (n == 0) {                       /* untextured: the vertex colour */
        pica_tev_passthrough(&out[0]);
        out[0].src_rgb[0] = out[0].src_a[0] = PICA_TEVSRC_PRIMARY;
        n = 1;
    }
    for (int i = n; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&out[i]);
    return n;
}

void pica_ff_tev_bgra_swizzle(pica_tev_t out[PICA_TEV_STAGES])
{
    /* An RGBA8 buffer is stored A,B,G,R; the iOS framebuffer is B,G,R,A.
       So write (R', G', B', A') = (A, R, G, B): one channel per stage,
       each picked with a per-channel operand and a one-hot constant. */
    static const uint8_t pick[3] = { PICA_TEVOP_RGB_ALPHA, PICA_TEVOP_RGB_R, PICA_TEVOP_RGB_G };

    for (int i = 0; i < PICA_TEV_STAGES; i++)
        pica_tev_passthrough(&out[i]);
    for (int i = 0; i < 3; i++) {
        pica_tev_t *t = &out[i];
        t->src_rgb[0] = PICA_TEVSRC_TEXTURE0;
        t->op_rgb[0] = pick[i];
        t->src_rgb[1] = PICA_TEVSRC_CONSTANT;
        t->color = 0xFFu << (8 * i);                    /* red, green, blue */
        if (i == 0) {
            t->func_rgb = PICA_TEV_MODULATE;
            t->src_a[0] = PICA_TEVSRC_TEXTURE0;         /* A' = B */
            t->op_a[0] = PICA_TEVOP_A_B;
        } else {
            t->src_rgb[2] = PICA_TEVSRC_PREVIOUS;
            t->func_rgb = PICA_TEV_MULT_ADD;            /* a * b + c */
        }
    }
}

/* ---- enum translation ---- */

int pica_gl_func(uint32_t f)
{
    /* GL_NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS */
    static const int8_t map[8] = { PICA_NEVER, PICA_LESS, PICA_EQUAL, PICA_LEQUAL,
                                   PICA_GREATER, PICA_NOTEQUAL, PICA_GEQUAL, PICA_ALWAYS };
    return f - GL_NEVER < 8 ? map[f - GL_NEVER] : -1;
}

int pica_gl_blend_factor(uint32_t f)
{
    /* GL_SRC_COLOR .. GL_SRC_ALPHA_SATURATE */
    static const int8_t map[9] = {
        PICA_BF_SRC_COLOR, PICA_BF_INV_SRC_COLOR, PICA_BF_SRC_ALPHA, PICA_BF_INV_SRC_ALPHA,
        PICA_BF_DST_ALPHA, PICA_BF_INV_DST_ALPHA, PICA_BF_DST_COLOR, PICA_BF_INV_DST_COLOR,
        PICA_BF_SRC_ALPHA_SAT };
    if (f == GL_ZERO) return PICA_BF_ZERO;
    if (f == GL_ONE) return PICA_BF_ONE;
    return f - GL_SRC_COLOR < 9 ? map[f - GL_SRC_COLOR] : -1;
}

int pica_gl_blend_eq(uint32_t e)
{
    switch (e) {
    case GL_FUNC_ADD: return PICA_BLEND_ADD;
    case GL_FUNC_SUBTRACT: return PICA_BLEND_SUB;
    case GL_FUNC_REVERSE_SUBTRACT: return PICA_BLEND_RSUB;
    case GL_MIN: return PICA_BLEND_MIN;
    case GL_MAX: return PICA_BLEND_MAX;
    }
    return -1;
}

int pica_gl_stencil_op(uint32_t op)
{
    switch (op) {
    case GL_KEEP: return PICA_STENCIL_KEEP;
    case GL_ZERO: return PICA_STENCIL_ZERO;
    case GL_REPLACE: return PICA_STENCIL_REPLACE;
    case GL_INCR: return PICA_STENCIL_INCR;
    case GL_DECR: return PICA_STENCIL_DECR;
    case GL_INVERT: return PICA_STENCIL_INVERT;
    case GL_INCR_WRAP: return PICA_STENCIL_INCR_WRAP;
    case GL_DECR_WRAP: return PICA_STENCIL_DECR_WRAP;
    }
    return -1;
}

int pica_gl_logic_op(uint32_t op)
{
    /* GL_CLEAR, AND, AND_REVERSE, COPY, AND_INVERTED, NOOP, XOR, OR, NOR, EQUIV,
       INVERT, OR_REVERSE, COPY_INVERTED, OR_INVERTED, NAND, SET */
    static const int8_t map[16] = { 0, 1, 2, 3, 13, 6, 11, 9, 10, 12, 7, 14, 5, 15, 8, 4 };
    return op - GL_CLEAR < 16 ? map[op - GL_CLEAR] : -1;
}

int pica_gl_wrap(uint32_t w)
{
    switch (w) {
    case GL_REPEAT: return PICA_REPEAT;
    case GL_CLAMP_TO_EDGE: return PICA_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return PICA_MIRRORED_REPEAT;
    }
    return -1;
}

int pica_gl_prim(uint32_t mode)
{
    switch (mode) {
    case GL_TRIANGLES: return PICA_TRIANGLES;
    case GL_TRIANGLE_STRIP: return PICA_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return PICA_TRIANGLE_FAN;
    }
    return -1;      /* points and lines: the engine draws them as triangles */
}

int pica_gl_attr_type(uint32_t type)
{
    switch (type) {
    case GL_BYTE: return PICA_BYTE;
    case GL_UNSIGNED_BYTE: return PICA_UBYTE;
    case GL_SHORT: return PICA_SHORT;
    case GL_FLOAT: return PICA_FLOAT;
    }
    return -1;
}
