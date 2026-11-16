/* ios3ds GL engine: the software Backend.
 *
 * Takes clip-space triangles and the GL raster state and writes ARGB32
 * pixels: clipping, perspective divide, viewport, culling, a scanline
 * rasteriser (top-left rule, pixel centres at +0.5), perspective-correct
 * texturing (16-pixel affine sub-spans), two texture units with the ES 1.1
 * environment modes incl. GL_COMBINE, fog, alpha test, 16-bit depth, blending.
 *
 * This file is what a PICA200 back end replaces (docs/gpu.md).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define NATTR (7 + 2 * MAX_TEX_UNITS)
enum { A_Z, A_R, A_G, A_B, A_A, A_FOG, A_IW, A_U0, A_V0 };
#define ZSHIFT 14
#define GUARD 8.0f

typedef struct TS {
    const uint32_t *px;
    int w, h, pitch;
    int linear, wrap_s, wrap_t;         /* 0 repeat, 1 clamp, 2 mirror */
    int has_rgb, has_a;
    GLenum mode;
    const TexEnv *env;
    int cc[4];
} TS;

typedef struct RS {
    RT rt;
    int cx0, cy0, cx1, cy1;             /* clip rectangle, memory coordinates */
    int ntex;
    int unit[MAX_TEX_UNITS];            /* GL unit number of ts[k] */
    TS ts[MAX_TEX_UNITS];
    int depth_test, depth_write, alpha_test, blend, fog, fast;
    GLenum depth_func, alpha_func;
    int alpha_ref;
    GLenum bs_rgb, bd_rgb, bs_a, bd_a, beq_rgb, beq_a;
    int fogc[3];
    uint32_t wmask;
    float vx, vy, hw, hh, zs, zo;       /* viewport transform */
    int cull;                           /* 0 none, 1 cull CCW (GL space), -1 cull CW, 2 all */
    float poly_factor, poly_units;
} RS;

typedef struct SV {
    float x, y;                         /* memory coordinates */
    float a[NATTR];
} SV;

static inline int iceil(float f)
{
    int i = (int)f;
    return i + (f > (float)i);
}

static inline int wrap_coord(int x, int n, int mode)
{
    if ((unsigned)x < (unsigned)n)
        return x;
    if (mode == 1)
        return x < 0 ? 0 : n - 1;
    if (mode == 2) {
        int m = x % (2 * n);
        if (m < 0)
            m += 2 * n;
        return m < n ? m : 2 * n - 1 - m;
    }
    x %= n;
    return x < 0 ? x + n : x;
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, unsigned f)     /* f 0..256 */
{
    uint32_t rb = a & 0x00ff00ff, ag = (a >> 8) & 0x00ff00ff;
    rb += (((b & 0x00ff00ff) - rb) * f) >> 8;
    ag += ((((b >> 8) & 0x00ff00ff) - ag) * f) >> 8;
    return (rb & 0x00ff00ff) | ((ag & 0x00ff00ff) << 8);
}

static inline uint32_t tex_fetch(const TS *t, int32_t u, int32_t v)   /* 16.16 texel coordinates */
{
    if (t->linear) {
        int x0, y0, x1, y1;
        unsigned fx, fy;
        const uint32_t *r0, *r1;
        u -= 0x8000;
        v -= 0x8000;
        x0 = u >> 16; y0 = v >> 16;
        fx = (u >> 8) & 0xff; fy = (v >> 8) & 0xff;
        x1 = wrap_coord(x0 + 1, t->w, t->wrap_s);
        y1 = wrap_coord(y0 + 1, t->h, t->wrap_t);
        x0 = wrap_coord(x0, t->w, t->wrap_s);
        y0 = wrap_coord(y0, t->h, t->wrap_t);
        r0 = t->px + (size_t)y0 * t->pitch;
        r1 = t->px + (size_t)y1 * t->pitch;
        return lerp_px(lerp_px(r0[x0], r0[x1], fx), lerp_px(r1[x0], r1[x1], fx), fy);
    }
    return t->px[(size_t)wrap_coord(v >> 16, t->h, t->wrap_t) * t->pitch + wrap_coord(u >> 16, t->w, t->wrap_s)];
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int mul255(int a, int b) { int t = a * b + 128; return (t + (t >> 8)) >> 8; }

/* GL_COMBINE for one unit. f = previous (in/out), tx = texel, pr = primary colour. */
static void combine(const TS *t, int *f, const int *tx, const int *pr)
{
    const TexEnv *e = t->env;
    int arg[3][4], k, c, out[4];
    for (k = 0; k < 3; k++) {
        const int *s;
        int alpha;
        switch (e->src_rgb[k]) {
        case GL_TEXTURE: s = tx; break;
        case GL_CONSTANT: s = t->cc; break;
        case GL_PRIMARY_COLOR: s = pr; break;
        default: s = f; break;
        }
        alpha = s[3];
        switch (e->op_rgb[k]) {
        case GL_ONE_MINUS_SRC_COLOR: arg[k][0] = 255 - s[0]; arg[k][1] = 255 - s[1]; arg[k][2] = 255 - s[2]; break;
        case GL_SRC_ALPHA: arg[k][0] = arg[k][1] = arg[k][2] = alpha; break;
        case GL_ONE_MINUS_SRC_ALPHA: arg[k][0] = arg[k][1] = arg[k][2] = 255 - alpha; break;
        default: arg[k][0] = s[0]; arg[k][1] = s[1]; arg[k][2] = s[2]; break;
        }
        switch (e->src_a[k]) {
        case GL_TEXTURE: s = tx; break;
        case GL_CONSTANT: s = t->cc; break;
        case GL_PRIMARY_COLOR: s = pr; break;
        default: s = f; break;
        }
        arg[k][3] = e->op_a[k] == GL_ONE_MINUS_SRC_ALPHA ? 255 - s[3] : s[3];
    }
    for (c = 0; c < 4; c++) {
        GLenum fn = c < 3 ? e->comb_rgb : e->comb_a;
        int a0 = arg[0][c], a1 = arg[1][c], a2 = arg[2][c], r;
        switch (fn) {
        case GL_REPLACE:     r = a0; break;
        case GL_ADD:         r = a0 + a1; break;
        case GL_ADD_SIGNED:  r = a0 + a1 - 128; break;
        case GL_SUBTRACT:    r = a0 - a1; break;
        case GL_INTERPOLATE: r = mul255(a0, a2) + mul255(a1, 255 - a2); break;
        case GL_DOT3_RGB: case GL_DOT3_RGBA:
            r = ((arg[0][0] - 128) * (arg[1][0] - 128) + (arg[0][1] - 128) * (arg[1][1] - 128) +
                 (arg[0][2] - 128) * (arg[1][2] - 128)) >> 5;
            break;
        default:             r = mul255(a0, a1); break;      /* GL_MODULATE */
        }
        if (c < 3 && e->rgb_scale != 1)
            r = (int)(r * e->rgb_scale);
        if (c == 3 && e->a_scale != 1)
            r = (int)(r * e->a_scale);
        out[c] = clamp255(r);
    }
    if (e->comb_rgb == GL_DOT3_RGBA)
        out[3] = out[0];
    memcpy(f, out, sizeof out);
}

static inline void texenv(const TS *t, int *f, uint32_t texel, const int *pr)
{
    int tx[4] = {(texel >> 16) & 255, (texel >> 8) & 255, texel & 255, texel >> 24};
    switch (t->mode) {
    case GL_REPLACE:
        if (t->has_rgb) { f[0] = tx[0]; f[1] = tx[1]; f[2] = tx[2]; }
        if (t->has_a) f[3] = tx[3];
        break;
    case GL_DECAL:
        if (t->has_rgb && t->has_a) {
            f[0] += mul255(tx[0] - f[0], tx[3]);
            f[1] += mul255(tx[1] - f[1], tx[3]);
            f[2] += mul255(tx[2] - f[2], tx[3]);
        } else if (t->has_rgb) { f[0] = tx[0]; f[1] = tx[1]; f[2] = tx[2]; }
        break;
    case GL_BLEND:
        if (t->has_rgb) {
            f[0] += mul255(t->cc[0] - f[0], tx[0]);
            f[1] += mul255(t->cc[1] - f[1], tx[1]);
            f[2] += mul255(t->cc[2] - f[2], tx[2]);
        }
        if (t->has_a) f[3] = mul255(f[3], tx[3]);
        break;
    case GL_ADD:
        if (t->has_rgb) {
            f[0] = clamp255(f[0] + tx[0]); f[1] = clamp255(f[1] + tx[1]); f[2] = clamp255(f[2] + tx[2]);
        }
        if (t->has_a) f[3] = mul255(f[3], tx[3]);
        break;
    case GL_COMBINE:
        if (!t->has_rgb) tx[0] = tx[1] = tx[2] = 0;
        if (!t->has_a) tx[3] = 255;
        combine(t, f, tx, pr);
        break;
    default:                    /* GL_MODULATE */
        if (t->has_rgb) {
            f[0] = mul255(f[0], tx[0]); f[1] = mul255(f[1], tx[1]); f[2] = mul255(f[2], tx[2]);
        }
        if (t->has_a) f[3] = mul255(f[3], tx[3]);
        break;
    }
}

static inline int cmp(GLenum fn, int a, int b)
{
    switch (fn) {
    case GL_NEVER:    return 0;
    case GL_LESS:     return a < b;
    case GL_EQUAL:    return a == b;
    case GL_LEQUAL:   return a <= b;
    case GL_GREATER:  return a > b;
    case GL_NOTEQUAL: return a != b;
    case GL_GEQUAL:   return a >= b;
    }
    return 1;
}

/* Blend factor for all four channels, 0..256. */
static inline void bfactor(GLenum f, const int *s, const int *d, int *o, int alpha_ch)
{
    int v;
    switch (f) {
    case GL_ZERO: o[0] = o[1] = o[2] = o[3] = 0; return;
    case GL_ONE:  o[0] = o[1] = o[2] = o[3] = 256; return;
    case GL_SRC_COLOR: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    case GL_ONE_MINUS_SRC_COLOR: o[0] = 255 - s[0]; o[1] = 255 - s[1]; o[2] = 255 - s[2]; o[3] = 255 - s[3]; break;
    case GL_DST_COLOR: o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; o[3] = d[3]; break;
    case GL_ONE_MINUS_DST_COLOR: o[0] = 255 - d[0]; o[1] = 255 - d[1]; o[2] = 255 - d[2]; o[3] = 255 - d[3]; break;
    case GL_SRC_ALPHA: o[0] = o[1] = o[2] = o[3] = s[3]; break;
    case GL_ONE_MINUS_SRC_ALPHA: o[0] = o[1] = o[2] = o[3] = 255 - s[3]; break;
    case GL_DST_ALPHA: o[0] = o[1] = o[2] = o[3] = d[3]; break;
    case GL_ONE_MINUS_DST_ALPHA: o[0] = o[1] = o[2] = o[3] = 255 - d[3]; break;
    case GL_SRC_ALPHA_SATURATE:
        v = s[3] < 255 - d[3] ? s[3] : 255 - d[3];
        o[0] = o[1] = o[2] = v; o[3] = 255;
        break;
    default: o[0] = o[1] = o[2] = o[3] = 256; return;
    }
    (void)alpha_ch;
    o[0] += o[0] >> 7; o[1] += o[1] >> 7; o[2] += o[2] >> 7; o[3] += o[3] >> 7;
}

static inline int beq(GLenum eq, int s, int sf, int d, int df)
{
    switch (eq) {
    case GL_FUNC_SUBTRACT:         return clamp255((s * sf - d * df) >> 8);
    case GL_FUNC_REVERSE_SUBTRACT: return clamp255((d * df - s * sf) >> 8);
    case GL_MIN_EXT:               return s < d ? s : d;
    case GL_MAX_EXT:               return s > d ? s : d;
    }
    return clamp255((s * sf + d * df) >> 8);
}

/* Turn float texel coordinates + step into 16.16 fixed, keeping them small. */
static inline void fix_uv(const TS *t, float u, float v, float du, float dv, int32_t *o)
{
    if (t->wrap_s == 0)
        u -= floorf(u / t->w) * t->w;
    else if (u > 16000 || u < -16000)
        u = u > 0 ? 16000 : -16000;
    if (t->wrap_t == 0)
        v -= floorf(v / t->h) * t->h;
    else if (v > 16000 || v < -16000)
        v = v > 0 ? 16000 : -16000;
    o[0] = (int32_t)(u * 65536.0f);
    o[1] = (int32_t)(v * 65536.0f);
    o[2] = (int32_t)(du * 65536.0f);
    o[3] = (int32_t)(dv * 65536.0f);
}

/* One horizontal run of pixels [x0, x1) on row y. a = attributes at the
 * centre of pixel x0, dx = d/dx. persp: A_U/A_V hold u/w, v/w and A_IW 1/w. */
static void draw_span(const RS *rs, int y, int x0, int x1, const float *a, const float *dx, int persp)
{
    uint32_t *dst = rs->rt.color + (size_t)y * rs->rt.pitch + x0;
    uint16_t *zb = rs->rt.depth ? rs->rt.depth + (size_t)y * rs->rt.dpitch + x0 : NULL;
    int32_t z = (int32_t)(a[A_Z] * (float)(1 << ZSHIFT)), dz = (int32_t)(dx[A_Z] * (float)(1 << ZSHIFT));
    int32_t c[4], dc[4], fog = (int32_t)(a[A_FOG] * 65536.0f), dfog = (int32_t)(dx[A_FOG] * 65536.0f);
    int32_t uv[MAX_TEX_UNITS][4];
    int k, n, x = x0, ntex = rs->ntex;
    float iw = a[A_IW];

    for (k = 0; k < 4; k++) {
        c[k] = (int32_t)(a[A_R + k] * 65536.0f);
        dc[k] = (int32_t)(dx[A_R + k] * 65536.0f);
    }
    if (!persp)
        for (k = 0; k < ntex; k++)
            fix_uv(&rs->ts[k], a[A_U0 + 2 * k], a[A_V0 + 2 * k], dx[A_U0 + 2 * k], dx[A_V0 + 2 * k], uv[k]);

    while (x < x1) {
        n = x1 - x;
        if (persp) {
            float w0, w1, iw1;
            if (n > 16)
                n = 16;
            iw1 = iw + dx[A_IW] * n;
            w0 = 1.0f / iw;
            w1 = 1.0f / iw1;
            for (k = 0; k < ntex; k++) {
                float s0 = a[A_U0 + 2 * k] + dx[A_U0 + 2 * k] * (x - x0), t0 = a[A_V0 + 2 * k] + dx[A_V0 + 2 * k] * (x - x0);
                float s1 = s0 + dx[A_U0 + 2 * k] * n, t1 = t0 + dx[A_V0 + 2 * k] * n;
                float inv = 1.0f / n;
                fix_uv(&rs->ts[k], s0 * w0, t0 * w0, (s1 * w1 - s0 * w0) * inv, (t1 * w1 - t0 * w0) * inv, uv[k]);
            }
            iw = iw1;
        }
        for (; n > 0; n--, x++, dst++) {
            int f[4], pr[4];
            uint32_t out;
            unsigned zz = 0;
            if (rs->depth_test) {
                zz = (unsigned)z >> ZSHIFT;
                if (zz > 65535)
                    zz = z < 0 ? 0 : 65535;
                if (!cmp(rs->depth_func, (int)zz, *zb))
                    goto next;
            }
            f[0] = c[0] >> 16; f[1] = c[1] >> 16; f[2] = c[2] >> 16; f[3] = c[3] >> 16;
            if ((unsigned)(f[0] | f[1] | f[2] | f[3]) > 255) {
                f[0] = clamp255(f[0]); f[1] = clamp255(f[1]); f[2] = clamp255(f[2]); f[3] = clamp255(f[3]);
            }
            if (ntex) {
                pr[0] = f[0]; pr[1] = f[1]; pr[2] = f[2]; pr[3] = f[3];
                texenv(&rs->ts[0], f, tex_fetch(&rs->ts[0], uv[0][0], uv[0][1]), pr);
                if (ntex > 1)
                    texenv(&rs->ts[1], f, tex_fetch(&rs->ts[1], uv[1][0], uv[1][1]), pr);
            }
            if (rs->fog) {
                int ff = fog >> 8;
                ff = ff < 0 ? 0 : ff > 256 ? 256 : ff;
                f[0] = rs->fogc[0] + (((f[0] - rs->fogc[0]) * ff) >> 8);
                f[1] = rs->fogc[1] + (((f[1] - rs->fogc[1]) * ff) >> 8);
                f[2] = rs->fogc[2] + (((f[2] - rs->fogc[2]) * ff) >> 8);
            }
            if (rs->alpha_test && !cmp(rs->alpha_func, f[3], rs->alpha_ref))
                goto next;
            if (rs->depth_write)
                *zb = (uint16_t)zz;
            if (rs->blend) {
                uint32_t dp = *dst;
                int d[4] = {(dp >> 16) & 255, (dp >> 8) & 255, dp & 255, dp >> 24}, sf[4], df[4];
                if (rs->fast == 1) {            /* ONE, ONE_MINUS_SRC_ALPHA */
                    int ia = 256 - (f[3] + (f[3] >> 7));
                    f[0] = clamp255(f[0] + ((d[0] * ia) >> 8));
                    f[1] = clamp255(f[1] + ((d[1] * ia) >> 8));
                    f[2] = clamp255(f[2] + ((d[2] * ia) >> 8));
                    f[3] = clamp255(f[3] + ((d[3] * ia) >> 8));
                } else if (rs->fast == 2) {     /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
                    int sa = f[3] + (f[3] >> 7);
                    f[0] = d[0] + (((f[0] - d[0]) * sa) >> 8);
                    f[1] = d[1] + (((f[1] - d[1]) * sa) >> 8);
                    f[2] = d[2] + (((f[2] - d[2]) * sa) >> 8);
                    f[3] = d[3] + (((f[3] - d[3]) * sa) >> 8);
                } else {
                    int sfa[4], dfa[4];
                    bfactor(rs->bs_rgb, f, d, sf, 0);
                    bfactor(rs->bd_rgb, f, d, df, 0);
                    bfactor(rs->bs_a, f, d, sfa, 1);
                    bfactor(rs->bd_a, f, d, dfa, 1);
                    f[0] = beq(rs->beq_rgb, f[0], sf[0], d[0], df[0]);
                    f[1] = beq(rs->beq_rgb, f[1], sf[1], d[1], df[1]);
                    f[2] = beq(rs->beq_rgb, f[2], sf[2], d[2], df[2]);
                    f[3] = beq(rs->beq_a, f[3], sfa[3], d[3], dfa[3]);
                }
            }
            out = ((uint32_t)f[3] << 24) | ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | (uint32_t)f[2];
            if (rs->wmask != 0xffffffffu)
                out = (out & rs->wmask) | (*dst & ~rs->wmask);
            *dst = out | rs->rt.opaque;
next:
            z += dz;
            c[0] += dc[0]; c[1] += dc[1]; c[2] += dc[2]; c[3] += dc[3];
            fog += dfog;
            for (k = 0; k < ntex; k++) {
                uv[k][0] += uv[k][2];
                uv[k][1] += uv[k][3];
            }
            if (zb)
                zb++;
        }
    }
}

static void project(const RS *rs, const Vtx *v, SV *o, int persp)
{
    float iw = 1.0f / v->w;
    float gy = rs->vy + (v->y * iw + 1.0f) * rs->hh;       /* GL window y (up) */
    int k;
    o->x = rs->vx + (v->x * iw + 1.0f) * rs->hw;
    o->y = rs->rt.flip ? (float)rs->rt.h - gy : gy;
    o->a[A_Z] = (v->z * iw * rs->zs + rs->zo) * 65535.0f;
    if (o->a[A_Z] < 0) o->a[A_Z] = 0;
    if (o->a[A_Z] > 65535.0f) o->a[A_Z] = 65535.0f;
    o->a[A_R] = v->c[0] * 255.0f;
    o->a[A_G] = v->c[1] * 255.0f;
    o->a[A_B] = v->c[2] * 255.0f;
    o->a[A_A] = v->c[3] * 255.0f;
    o->a[A_FOG] = v->fog * 256.0f;
    o->a[A_IW] = iw;
    for (k = 0; k < rs->ntex; k++) {
        float s = v->t[rs->unit[k]][0] * rs->ts[k].w, t = v->t[rs->unit[k]][1] * rs->ts[k].h;
        o->a[A_U0 + 2 * k] = persp ? s * iw : s;
        o->a[A_V0 + 2 * k] = persp ? t * iw : t;
    }
}

static void raster_tri(const RS *rs, const Vtx *va, const Vtx *vb, const Vtx *vc)
{
    SV sv[3], *v0 = &sv[0], *v1 = &sv[1], *v2 = &sv[2], *t;
    float area, inv, dadx[NATTR], dady[NATTR], row[NATTR];
    float d1x, d1y, d2x, d2y, e02, e01, e12;
    int persp, y, ya, yb, k, nattr = A_U0 + 2 * rs->ntex;

    persp = rs->ntex && (fabsf(va->w - vb->w) > 1e-5f * fabsf(va->w) || fabsf(va->w - vc->w) > 1e-5f * fabsf(va->w));
    project(rs, va, v0, persp);
    project(rs, vb, v1, persp);
    project(rs, vc, v2, persp);

    d1x = v1->x - v0->x; d1y = v1->y - v0->y;
    d2x = v2->x - v0->x; d2y = v2->y - v0->y;
    area = d1x * d2y - d2x * d1y;
    if (area == 0 || area != area)
        return;
    if (rs->cull) {
        /* 'area' is in memory space; a flipped target mirrors the winding */
        int ccw = rs->rt.flip ? area < 0 : area > 0;
        if (rs->cull == 2 || (rs->cull == 1 && ccw) || (rs->cull == -1 && !ccw))
            return;
    }
    inv = 1.0f / area;
    for (k = 0; k < nattr; k++) {
        float a1 = v1->a[k] - v0->a[k], a2 = v2->a[k] - v0->a[k];
        dadx[k] = (a1 * d2y - a2 * d1y) * inv;
        dady[k] = (a2 * d1x - a1 * d2x) * inv;
    }
    if (rs->poly_factor != 0 || rs->poly_units != 0) {
        float m = fabsf(dadx[A_Z]) > fabsf(dady[A_Z]) ? fabsf(dadx[A_Z]) : fabsf(dady[A_Z]);
        float off = rs->poly_factor * m + rs->poly_units;
        for (k = 0; k < 3; k++)
            sv[k].a[A_Z] += off;
    }

    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    if (v2->y < v0->y) { t = v0; v0 = v2; v2 = t; }
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    ya = iceil(v0->y - 0.5f);
    yb = iceil(v2->y - 0.5f);
    if (ya < rs->cy0) ya = rs->cy0;
    if (yb > rs->cy1) yb = rs->cy1;
    if (ya >= yb)
        return;
    e02 = (v2->x - v0->x) / (v2->y - v0->y);
    e01 = v1->y > v0->y ? (v1->x - v0->x) / (v1->y - v0->y) : 0;
    e12 = v2->y > v1->y ? (v2->x - v1->x) / (v2->y - v1->y) : 0;
    for (y = ya; y < yb; y++) {
        float yc = y + 0.5f, xa = v0->x + (yc - v0->y) * e02, xb, fx;
        int x0, x1;
        xb = yc < v1->y ? v0->x + (yc - v0->y) * e01 : v1->x + (yc - v1->y) * e12;
        if (xa > xb) { float s = xa; xa = xb; xb = s; }
        x0 = iceil(xa - 0.5f);
        x1 = iceil(xb - 0.5f);
        if (x0 < rs->cx0) x0 = rs->cx0;
        if (x1 > rs->cx1) x1 = rs->cx1;
        if (x0 >= x1)
            continue;
        fx = x0 + 0.5f - v0->x;
        for (k = 0; k < nattr; k++)
            row[k] = v0->a[k] + fx * dadx[k] + (yc - v0->y) * dady[k];
        draw_span(rs, y, x0, x1, row, dadx, persp);
    }
}

/* ------------------------------------------------------------ clipping */
static float plane_dist(const Vtx *v, int p)
{
    switch (p) {
    case 0: return v->w + v->z;
    case 1: return v->w - v->z;
    case 2: return GUARD * v->w + v->x;
    case 3: return GUARD * v->w - v->x;
    case 4: return GUARD * v->w + v->y;
    default: return GUARD * v->w - v->y;
    }
}

static void vlerp(Vtx *o, const Vtx *a, const Vtx *b, float t)
{
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *fo = (float *)o;
    unsigned i;
    for (i = 0; i < sizeof(Vtx) / sizeof(float); i++)
        fo[i] = fa[i] + (fb[i] - fa[i]) * t;
}

static void clip_tri(const RS *rs, const Vtx *tri)
{
    Vtx buf[2][12];
    const Vtx *in = tri;
    int n = 3, p, i, cur = 0;
    unsigned ormask = 0;
    /* trivial reject against the real frustum, trivial accept against the guard band */
    {
        unsigned andmask = ~0u;
        for (i = 0; i < 3; i++) {
            const Vtx *v = &tri[i];
            unsigned m = 0, g = 0;
            if (v->x < -v->w) m |= 1;
            if (v->x > v->w) m |= 2;
            if (v->y < -v->w) m |= 4;
            if (v->y > v->w) m |= 8;
            if (v->z < -v->w) m |= 16;
            if (v->z > v->w) m |= 32;
            for (p = 0; p < 6; p++)
                if (plane_dist(v, p) < 0)
                    g |= 1u << p;
            if (v->w <= 0)
                g |= 1;
            andmask &= m;
            ormask |= g;
        }
        if (andmask)
            return;
    }
    if (!ormask) {
        raster_tri(rs, &tri[0], &tri[1], &tri[2]);
        return;
    }
    for (p = 0; p < 6 && n >= 3; p++) {
        Vtx *out = buf[cur];
        int m = 0;
        if (!(ormask & (1u << p)))
            continue;
        for (i = 0; i < n; i++) {
            const Vtx *a = &in[i], *b = &in[(i + 1) % n];
            float da = plane_dist(a, p), db = plane_dist(b, p);
            if (da >= 0)
                out[m++] = *a;
            if ((da >= 0) != (db >= 0))
                vlerp(&out[m++], a, b, da / (da - db));
        }
        in = out;
        n = m;
        cur ^= 1;
    }
    for (i = 1; i + 1 < n; i++)
        if (in[0].w > 0 && in[i].w > 0 && in[i + 1].w > 0)
            raster_tri(rs, &in[0], &in[i], &in[i + 1]);
}

/* ------------------------------------------------------------- set-up */
static void clip_rect(GC *gc, const RT *rt, RS *rs)
{
    int x0 = 0, y0 = 0, x1 = rt->w, y1 = rt->h;
    if (gc->scissor_on) {
        int sx0 = gc->scissor[0], sy0 = gc->scissor[1], sx1 = sx0 + gc->scissor[2], sy1 = sy0 + gc->scissor[3];
        if (rt->flip) {
            int t = rt->h - sy1;
            sy1 = rt->h - sy0;
            sy0 = t;
        }
        if (sx0 > x0) x0 = sx0;
        if (sy0 > y0) y0 = sy0;
        if (sx1 < x1) x1 = sx1;
        if (sy1 < y1) y1 = sy1;
    }
    rs->cx0 = x0; rs->cy0 = y0; rs->cx1 = x1; rs->cy1 = y1;
}

static uint32_t write_mask(GC *gc)
{
    return (gc->color_mask[3] ? 0xff000000u : 0) | (gc->color_mask[0] ? 0x00ff0000u : 0) |
           (gc->color_mask[1] ? 0x0000ff00u : 0) | (gc->color_mask[2] ? 0x000000ffu : 0);
}

static int wrap_mode(GLenum w) { return w == GL_CLAMP_TO_EDGE ? 1 : w == GL_MIRRORED_REPEAT ? 2 : 0; }

static void sw_triangles(GC *gc, RT *rt, const Vtx *v, int n)
{
    RS rs;
    int u, i;
    memset(&rs, 0, sizeof rs);
    rs.rt = *rt;
    clip_rect(gc, rt, &rs);
    if (rs.cx0 >= rs.cx1 || rs.cy0 >= rs.cy1)
        return;
    rs.wmask = write_mask(gc);
    for (u = 0; u < MAX_TEX_UNITS; u++) {
        Tex *t = gc->tu[u].enabled ? gl_bound_tex(gc, u) : NULL;
        TS *ts = &rs.ts[rs.ntex];
        if (!t || !t->px || !t->w || !t->h)
            continue;
        ts->px = t->px; ts->w = t->w; ts->h = t->h; ts->pitch = t->pitch;
        ts->linear = t->mag_filter == GL_LINEAR;
        ts->wrap_s = wrap_mode(t->wrap_s);
        ts->wrap_t = wrap_mode(t->wrap_t);
        ts->has_rgb = t->base != GL_ALPHA;
        ts->has_a = t->base == GL_RGBA || t->base == GL_ALPHA || t->base == GL_LUMINANCE_ALPHA;
        ts->env = &gc->tu[u].env;
        ts->mode = ts->env->mode;
        for (i = 0; i < 4; i++)
            ts->cc[i] = (int)(ts->env->color[i] * 255.0f + 0.5f) & 0xff;
        rs.unit[rs.ntex++] = u;
    }
    rs.depth_test = gc->depth_test && rt->depth;
    rs.depth_func = gc->depth_func;
    rs.depth_write = rs.depth_test && gc->depth_mask;
    rs.alpha_test = gc->alpha_test && gc->alpha_func != GL_ALWAYS;
    rs.alpha_func = gc->alpha_func;
    rs.alpha_ref = (int)(gc->alpha_ref * 255.0f + 0.5f);
    rs.blend = gc->blend && !(gc->blend_src_rgb == GL_ONE && gc->blend_dst_rgb == GL_ZERO &&
                             gc->blend_src_a == GL_ONE && gc->blend_dst_a == GL_ZERO &&
                             gc->blend_eq_rgb == GL_FUNC_ADD && gc->blend_eq_a == GL_FUNC_ADD);
    rs.bs_rgb = gc->blend_src_rgb; rs.bd_rgb = gc->blend_dst_rgb;
    rs.bs_a = gc->blend_src_a; rs.bd_a = gc->blend_dst_a;
    rs.beq_rgb = gc->blend_eq_rgb; rs.beq_a = gc->blend_eq_a;
    if (rs.blend && rs.beq_rgb == GL_FUNC_ADD && rs.beq_a == GL_FUNC_ADD && rs.bs_rgb == rs.bs_a &&
        rs.bd_rgb == rs.bd_a && rs.bd_rgb == GL_ONE_MINUS_SRC_ALPHA)
        rs.fast = rs.bs_rgb == GL_ONE ? 1 : rs.bs_rgb == GL_SRC_ALPHA ? 2 : 0;
    rs.fog = gc->fog;
    for (i = 0; i < 3; i++)
        rs.fogc[i] = (int)(gc->fog_color[i] * 255.0f + 0.5f) & 0xff;
    rs.vx = gc->vp[0]; rs.vy = gc->vp[1];
    rs.hw = gc->vp[2] * 0.5f; rs.hh = gc->vp[3] * 0.5f;
    rs.zs = (gc->depth_far - gc->depth_near) * 0.5f;
    rs.zo = (gc->depth_far + gc->depth_near) * 0.5f;
    if (gc->cull) {
        int front_ccw = gc->front_face == GL_CCW;
        if (gc->cull_mode == GL_FRONT_AND_BACK)
            rs.cull = 2;
        else if (gc->cull_mode == GL_FRONT)
            rs.cull = front_ccw ? 1 : -1;
        else
            rs.cull = front_ccw ? -1 : 1;
    }
    if (gc->poly_offset_on) {
        rs.poly_factor = gc->poly_offset_factor;
        rs.poly_units = gc->poly_offset_units;
    }
    for (i = 0; i + 2 < n; i += 3)
        clip_tri(&rs, &v[i]);
}

static void sw_clear(GC *gc, RT *rt, GLbitfield mask)
{
    RS rs;
    int x, y;
    rs.rt = *rt;
    clip_rect(gc, rt, &rs);
    if (rs.cx0 >= rs.cx1 || rs.cy0 >= rs.cy1)
        return;
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t wm = write_mask(gc);
        uint32_t c = ((uint32_t)(gc->clear_color[3] * 255.0f + 0.5f) << 24) |
                     ((uint32_t)(gc->clear_color[0] * 255.0f + 0.5f) << 16) |
                     ((uint32_t)(gc->clear_color[1] * 255.0f + 0.5f) << 8) |
                     (uint32_t)(gc->clear_color[2] * 255.0f + 0.5f) | rt->opaque;
        for (y = rs.cy0; y < rs.cy1; y++) {
            uint32_t *d = rt->color + (size_t)y * rt->pitch;
            if (wm == 0xffffffffu)
                for (x = rs.cx0; x < rs.cx1; x++)
                    d[x] = c;
            else
                for (x = rs.cx0; x < rs.cx1; x++)
                    d[x] = (c & wm) | (d[x] & ~wm);
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && rt->depth && gc->depth_mask) {
        uint16_t zv = (uint16_t)(gc->clear_depth * 65535.0f + 0.5f);
        for (y = rs.cy0; y < rs.cy1; y++) {
            uint16_t *d = rt->depth + (size_t)y * rt->dpitch;
            for (x = rs.cx0; x < rs.cx1; x++)
                d[x] = zv;
        }
    }
}

const Backend sw_backend = {
    "ios3ds software rasterizer",
    sw_clear,
    sw_triangles,
    NULL,
    NULL,
    NULL,
};
