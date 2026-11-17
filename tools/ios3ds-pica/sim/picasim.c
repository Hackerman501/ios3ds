/*
 * picasim: software model of the PICA200 (see picasim.h, docs/pica200.md).
 *
 * Modelled: PSC fill 0/1, PPF display transfer + texture copy, the P3D
 * command-list processor, vertex arrays + fixed attributes, the vertex
 * shader (arithmetic subset), clipping, triangle lists/strips/fans, three
 * 2D texture units, six TEV stages with the combiner buffer, alpha test,
 * depth test, blending / logic op "copy", colour and depth masks, scissor.
 * Not modelled (counted in st.errors and logged): geometry shaders, shader
 * flow control, fragment lighting, fog, stencil, shadow/cube/procedural
 * textures, ETC1, mip levels, immediate-mode vertex submission.
 *
 * Deviations from the Citra software renderer this follows:
 *   - textures are filtered bilinearly when the magnification filter bit is
 *     set (Citra's software path is nearest only; the hardware filters)
 *   - all buffer addresses are bounds-checked through host.mem
 * SPDX-License-Identifier: GPL-2.0-or-later (so it can be linked into QEMU)
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "picasim.h"

/* GX MMIO word indices */
#define GX_PSC0      (0x010 / 4)
#define GX_PSC1      (0x020 / 4)
#define GX_IRQ_STAT  (0x034 / 4)
#define GX_PPF       (0xC00 / 4)
#define STAT_PSC0    (1u << 26)
#define STAT_PPF     (1u << 30)
#define STAT_P3D     (1u << 31)

static void sim_log(PicaSim *s, const char *fmt, uint32_t a, uint32_t b)
{
    char buf[160];
    s->st.errors++;
    if (!s->host.log || s->logged >= 32)
        return;
    s->logged++;
    snprintf(buf, sizeof buf, fmt, a, b);
    s->host.log(s->host.opaque, buf);
}

static uint8_t *mem(PicaSim *s, uint32_t pa, uint32_t len, int write)
{
    uint8_t *p = (len && s->host.mem) ? s->host.mem(s->host.opaque, pa, len, write) : NULL;
    if (!p && len)
        sim_log(s, "picasim: GPU access to unmapped memory pa=0x%08x len=0x%x", pa, len);
    return p;
}

static void raise_irq(PicaSim *s, int id)
{
    if (s->host.irq)
        s->host.irq(s->host.opaque, id);
}

static float f24(uint32_t v)
{
    uint32_t u;
    float f;
    v &= 0xFFFFFF;
    if (!(v & 0x7FFFFF))
        return 0.0f;
    u = (v >> 23) << 31 | (((v >> 16) & 0x7F) - 63 + 127) << 23 | (v & 0xFFFF) << 7;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t morton(uint32_t x, uint32_t y)
{
    static const uint8_t xl[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint8_t yl[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };
    return xl[x & 7] + yl[y & 7] + (x & ~7u) * 8;
}

/* byte offset of pixel (x, y) in an 8x8-tiled image `width` pixels wide */
static uint32_t tiled_off(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp)
{
    return (morton(x, y) + (y & ~7u) * width) * bpp;
}

/* ---- pixel formats (colour buffers, PPF): 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4 ---- */
static const uint8_t fb_bpp[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };

static void px_decode(unsigned fmt, const uint8_t *p, uint8_t c[4])
{
    unsigned v;
    switch (fmt) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; break;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; break;
    case 2:
        v = p[0] | p[1] << 8;
        c[0] = (v >> 11) * 255 / 31; c[1] = ((v >> 5) & 63) * 255 / 63; c[2] = (v & 31) * 255 / 31; c[3] = 255;
        break;
    case 3:
        v = p[0] | p[1] << 8;
        c[0] = (v >> 11) * 255 / 31; c[1] = ((v >> 6) & 31) * 255 / 31; c[2] = ((v >> 1) & 31) * 255 / 31;
        c[3] = (v & 1) ? 255 : 0;
        break;
    default:
        v = p[0] | p[1] << 8;
        c[0] = (v >> 12) * 17; c[1] = ((v >> 8) & 15) * 17; c[2] = ((v >> 4) & 15) * 17; c[3] = (v & 15) * 17;
        break;
    }
}

static void px_encode(unsigned fmt, uint8_t *p, const uint8_t c[4])
{
    unsigned v;
    switch (fmt) {
    case 0: p[3] = c[0]; p[2] = c[1]; p[1] = c[2]; p[0] = c[3]; break;
    case 1: p[2] = c[0]; p[1] = c[1]; p[0] = c[2]; break;
    case 2: v = (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3; p[0] = v; p[1] = v >> 8; break;
    case 3: v = (c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | c[3] >> 7; p[0] = v; p[1] = v >> 8; break;
    default: v = (c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 | c[3] >> 4; p[0] = v; p[1] = v >> 8; break;
    }
}

/* ------------------------------------------------------------------ */
/* PSC memory fill, PPF                                                */
/* ------------------------------------------------------------------ */

static void psc_fill(PicaSim *s, int n)
{
    uint32_t *r = &s->gx[n ? GX_PSC1 : GX_PSC0];
    uint32_t start = r[0] << 3, end = r[1] << 3, val = r[2], i;
    unsigned width = (r[3] >> 8) & 3;
    uint8_t *p;
    s->st.fills++;
    if (end > start && (p = mem(s, start, end - start, 1))) {
        uint32_t len = end - start;
        if (width == 2)
            for (i = 0; i + 4 <= len; i += 4)
                memcpy(p + i, &val, 4);
        else if (width == 1)
            for (i = 0; i + 3 <= len; i += 3) {
                p[i] = val; p[i + 1] = val >> 8; p[i + 2] = val >> 16;
            }
        else
            for (i = 0; i + 2 <= len; i += 2) {
                p[i] = val; p[i + 1] = val >> 8;
            }
    }
    r[3] = (r[3] & ~1u) | 2;                    /* not busy, finished */
    s->gx[GX_IRQ_STAT] |= STAT_PSC0 << n;
    raise_irq(s, 40 + n);
}

static void ppf_run(PicaSim *s)
{
    uint32_t *r = &s->gx[GX_PPF];
    uint32_t src = r[0] << 3, dst = r[1] << 3, flags = r[4];
    if (flags & 8) {                            /* texture copy */
        uint32_t size = r[8] & ~15u;
        uint32_t iw = (r[9] & 0xFFFF) * 16, ig = (r[9] >> 16) * 16;
        uint32_t ow = (r[10] & 0xFFFF) * 16, og = (r[10] >> 16) * 16;
        uint8_t *sp, *dp;
        s->st.texcopies++;
        if (!ig) iw = size;
        if (!og) ow = size;
        if (size && iw && ow) {
            uint32_t slen = size / iw * (iw + ig), dlen = size / ow * (ow + og);
            if ((sp = mem(s, src, slen ? slen : size, 0)) && (dp = mem(s, dst, dlen ? dlen : size, 1))) {
                uint32_t ri = iw, ro = ow;
                while (size) {
                    uint32_t n = ri < ro ? ri : ro;
                    if (n > size) n = size;
                    memmove(dp, sp, n);
                    sp += n; dp += n; ri -= n; ro -= n; size -= n;
                    if (!ri) { ri = iw; sp += ig; }
                    if (!ro) { ro = ow; dp += og; }
                }
            }
        }
    } else {                                    /* display transfer */
        uint32_t ow = r[2] & 0xFFFF, oh = r[2] >> 16, iw = r[3] & 0xFFFF, ih = r[3] >> 16;
        unsigned ifmt = (flags >> 8) & 7, ofmt = (flags >> 12) & 7, scale = (flags >> 24) & 3;
        unsigned sbpp = fb_bpp[ifmt], dbpp = fb_bpp[ofmt];
        int in_linear = (flags >> 1) & 1, raw = (flags >> 5) & 1, flip = flags & 1;
        unsigned hs = scale ? 1 : 0, vs = scale == 2 ? 1 : 0;
        uint8_t *sp, *dp;
        uint32_t x, y;
        s->st.transfers++;
        if (ifmt > 4 || ofmt > 4 || scale > 2 || (flags & (1u << 16)) || (in_linear && scale))
            sim_log(s, "picasim: unsupported display transfer flags 0x%08x%.0u", flags, 0);
        else if (ow && oh && iw && ih) {
            if (flip && (flags & 4))
                dst += (iw - ow) * (oh - 1) * dbpp;
            ow >>= hs;
            oh >>= vs;
            sp = mem(s, src, iw * ih * sbpp, 0);
            dp = mem(s, dst, ow * oh * dbpp, 1);
            for (y = 0; sp && dp && y < oh; y++)
                for (x = 0; x < ow; x++) {
                    uint32_t ix = x << hs, iy = y << vs, oy = flip ? oh - 1 - y : y, so, d_o;
                    uint8_t c[4];
                    if (in_linear) {
                        so = (ix + iy * iw) * sbpp;
                        d_o = raw ? (x + oy * ow) * dbpp : tiled_off(x, oy, ow, dbpp);
                    } else {
                        so = tiled_off(ix, iy, iw, sbpp);
                        d_o = raw ? tiled_off(x, oy, ow, dbpp) : (x + oy * ow) * dbpp;
                    }
                    px_decode(ifmt, sp + so, c);
                    if (scale) {                /* box filter over the next 1 / 3 stored pixels */
                        unsigned k, n = scale == 1 ? 2 : 4, sum[4] = { c[0], c[1], c[2], c[3] };
                        uint8_t t[4];
                        for (k = 1; k < n; k++) {
                            px_decode(ifmt, sp + so + k * sbpp, t);
                            sum[0] += t[0]; sum[1] += t[1]; sum[2] += t[2]; sum[3] += t[3];
                        }
                        for (k = 0; k < 4; k++)
                            c[k] = sum[k] / n;
                    }
                    px_encode(ofmt, dp + d_o, c);
                }
        }
    }
    r[6] = (r[6] & ~1u) | 0x100;                /* finished */
    s->gx[GX_IRQ_STAT] |= STAT_PPF;
    raise_irq(s, 44);
}

/* ------------------------------------------------------------------ */
/* Vertex shader                                                       */
/* ------------------------------------------------------------------ */

typedef struct { float in[16][4], tmp[16][4], out[16][4]; } ShaderRegs;

static const float *sh_src(PicaSim *s, ShaderRegs *r, unsigned n)
{
    if (n < 0x10) return r->in[n];
    if (n < 0x20) return r->tmp[n - 0x10];
    return s->uni[(n - 0x20) % 96];
}

static void sh_swz(float d[4], const float *v, unsigned sel, int neg)
{
    int i;
    for (i = 0; i < 4; i++) {
        float f = v[(sel >> (6 - 2 * i)) & 3];
        d[i] = neg ? -f : f;
    }
}

static void run_shader(PicaSim *s, ShaderRegs *r)
{
    unsigned pc = s->reg[0x2BA] & 0x1FF, steps;
    memset(r->tmp, 0, sizeof r->tmp);
    for (steps = 0; steps < 2048; steps++, pc = (pc + 1) & 0x1FF) {
        uint32_t ins = s->code[pc], op = ins >> 26, d = s->opdesc[ins & 0x7F];
        unsigned dst = (ins >> 21) & 0x1F, s1 = (ins >> 12) & 0x7F, s2 = (ins >> 7) & 0x1F;
        float a[4], b[4], o[4], *dp, t;
        int i;
        if (op == 0x22)                         /* END */
            return;
        if (op == 0x21)                         /* NOP */
            continue;
        sh_swz(a, sh_src(s, r, s1), (d >> 5) & 0xFF, (d >> 4) & 1);
        sh_swz(b, sh_src(s, r, s2), (d >> 14) & 0xFF, (d >> 13) & 1);
        switch (op) {
        case 0x00: for (i = 0; i < 4; i++) o[i] = a[i] + b[i]; break;                       /* ADD */
        case 0x01: t = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; o[0] = o[1] = o[2] = o[3] = t; break;
        case 0x02: t = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; o[0] = o[1] = o[2] = o[3] = t; break;
        case 0x08: for (i = 0; i < 4; i++) o[i] = a[i] * b[i]; break;                       /* MUL */
        case 0x09: for (i = 0; i < 4; i++) o[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;        /* SGE */
        case 0x0A: for (i = 0; i < 4; i++) o[i] = a[i] < b[i] ? 1.0f : 0.0f; break;         /* SLT */
        case 0x0B: for (i = 0; i < 4; i++) o[i] = floorf(a[i]); break;                      /* FLR */
        case 0x0C: for (i = 0; i < 4; i++) o[i] = a[i] > b[i] ? a[i] : b[i]; break;         /* MAX */
        case 0x0D: for (i = 0; i < 4; i++) o[i] = a[i] < b[i] ? a[i] : b[i]; break;         /* MIN */
        case 0x0E: o[0] = o[1] = o[2] = o[3] = 1.0f / a[0]; break;                          /* RCP */
        case 0x0F: o[0] = o[1] = o[2] = o[3] = 1.0f / sqrtf(a[0]); break;                   /* RSQ */
        case 0x13: memcpy(o, a, sizeof o); break;                                           /* MOV */
        default:
            sim_log(s, "picasim: unsupported shader opcode 0x%02x at %u", op, pc);
            return;
        }
        dp = dst < 0x10 ? r->out[dst] : r->tmp[dst - 0x10];
        for (i = 0; i < 4; i++)
            if (d & (8u >> i))
                dp[i] = o[i];
    }
    sim_log(s, "picasim: vertex shader did not reach END (entry %u)%.0u", s->reg[0x2BA] & 0x1FF, 0);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */

/* rasterizer input: position, colour, texcoords (semantic slots 0..23) */
typedef struct { float pos[4], col[4], tc[3][2]; float sx, sy, sz; } RVtx;

typedef struct {
    int on;
    const uint8_t *data;
    uint32_t w, h;
    unsigned fmt, wrap_s, wrap_t, linear;
    uint8_t border[4];
} TexUnit;

typedef struct {
    PicaSim *s;
    uint8_t *color, *depth;
    uint32_t fbw, fbh;              /* pixels */
    unsigned cfmt, cbpp, dfmt, dbpp;
    TexUnit tu[3];
} Target;

static const uint8_t tex_bits[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 0, 0 };

static void texel(const TexUnit *t, int x, int y, uint8_t c[4])
{
    uint32_t idx = morton(x, y) + ((uint32_t)y & ~7u) * t->w;       /* texel index */
    const uint8_t *p = t->data + idx * tex_bits[t->fmt] / 8;
    unsigned v;
    switch (t->fmt) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; break;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; break;
    case 2: px_decode(3, p, c); break;
    case 3: px_decode(2, p, c); break;
    case 4: px_decode(4, p, c); break;
    case 5: c[0] = c[1] = c[2] = p[1]; c[3] = p[0]; break;                  /* LA8 */
    case 6: c[0] = p[1]; c[1] = p[0]; c[2] = 0; c[3] = 255; break;          /* HILO8 */
    case 7: c[0] = c[1] = c[2] = p[0]; c[3] = 255; break;                   /* L8 */
    case 8: c[0] = c[1] = c[2] = 0; c[3] = p[0]; break;                     /* A8 */
    case 9: c[0] = c[1] = c[2] = (p[0] >> 4) * 17; c[3] = (p[0] & 15) * 17; break;
    case 10: v = (p[0] >> ((idx & 1) * 4)) & 15; c[0] = c[1] = c[2] = v * 17; c[3] = 255; break;
    default: v = (p[0] >> ((idx & 1) * 4)) & 15; c[0] = c[1] = c[2] = 0; c[3] = v * 17; break;
    }
}

/* returns 0 = use the border colour */
static int wrap(unsigned mode, int *v, int size)
{
    switch (mode & 7) {
    case 0: *v = *v < 0 ? 0 : *v >= size ? size - 1 : *v; return 1;
    case 1: return *v >= 0 && *v < size;
    case 3: {
        unsigned c = (unsigned)*v % (2u * size);
        *v = c >= (unsigned)size ? 2 * size - 1 - c : c;
        return 1;
    }
    default: *v = (unsigned)*v % (unsigned)size; return 1;
    }
}

static void tex_fetch(const TexUnit *t, int s, int tt, uint8_t c[4])
{
    if (!wrap(t->wrap_s, &s, t->w) || !wrap(t->wrap_t, &tt, t->h)) {
        memcpy(c, t->border, 4);
        return;
    }
    texel(t, s, (int)t->h - 1 - tt, c);         /* the first stored row is t = 1 */
}

static void tex_sample(const TexUnit *t, float u, float v, uint8_t c[4])
{
    if (!t->data) {
        c[0] = c[1] = c[2] = 0; c[3] = 255;
        return;
    }
    if (!t->linear) {
        tex_fetch(t, (int)(u * t->w), (int)(v * t->h), c);
    } else {
        float fu = u * t->w - 0.5f, fv = v * t->h - 0.5f;
        float iu = floorf(fu), iv = floorf(fv);
        int ax = (int)((fu - iu) * 256.0f), ay = (int)((fv - iv) * 256.0f), i;
        uint8_t q[4][4];
        tex_fetch(t, (int)iu, (int)iv, q[0]);
        tex_fetch(t, (int)iu + 1, (int)iv, q[1]);
        tex_fetch(t, (int)iu, (int)iv + 1, q[2]);
        tex_fetch(t, (int)iu + 1, (int)iv + 1, q[3]);
        for (i = 0; i < 4; i++) {
            int top = q[0][i] * (256 - ax) + q[1][i] * ax, bot = q[2][i] * (256 - ax) + q[3][i] * ax;
            c[i] = (uint8_t)((top * (256 - ay) + bot * ay + 32768) >> 16);
        }
    }
}

static int compare(unsigned func, uint32_t a, uint32_t b)
{
    switch (func & 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

static const uint16_t tev_base[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };

static void tev(PicaSim *s, const uint8_t prim[4], uint8_t tex[3][4], uint8_t out[4])
{
    uint8_t buf[4] = { 0, 0, 0, 0 }, next[4];
    uint32_t upd = s->reg[0x0E0], bc = s->reg[0x0FD];
    int st, i, k;
    next[0] = bc; next[1] = bc >> 8; next[2] = bc >> 16; next[3] = bc >> 24;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (st = 0; st < 6; st++) {
        const uint32_t *r = &s->reg[tev_base[st]];
        uint8_t kc[4] = { r[3], r[3] >> 8, r[3] >> 16, r[3] >> 24 };
        int rgb[3][3], al[3], res[4], cop = r[2] & 15, aop = (r[2] >> 16) & 15;
        int cmul = 1 << (r[4] & 3), amul = 1 << ((r[4] >> 16) & 3);
        for (k = 0; k < 3; k++) {
            unsigned cs = (r[0] >> (4 * k)) & 15, as = (r[0] >> (16 + 4 * k)) & 15;
            unsigned cm = (r[1] >> (4 * k)) & 15, am = (r[1] >> (12 + 4 * k)) & 7;
            const uint8_t *src[2];
            unsigned sel[2], j;
            /* the first stage has no "previous": sources 1 and 2 read source 3 instead */
            if (st == 0 && k < 2 && cs == 15)
                cs = (r[0] >> 8) & 15;
            sel[0] = cs; sel[1] = as;
            for (j = 0; j < 2; j++)
                switch (sel[j]) {
                case 0: src[j] = prim; break;
                case 3: case 4: case 5: src[j] = tex[sel[j] - 3]; break;
                case 13: src[j] = buf; break;
                case 14: src[j] = kc; break;
                case 15: src[j] = out; break;
                default: {
                    static const uint8_t zero[4];
                    src[j] = zero;          /* fragment lighting colours: not modelled */
                    break;
                }
                }
            for (i = 0; i < 3; i++) {
                int v = cm < 2 ? src[0][i] : cm < 4 ? src[0][3] : src[0][(cm >> 2) - 1];
                rgb[k][i] = (cm & 1) ? 255 - v : v;
            }
            {
                int v = am < 2 ? src[1][3] : src[1][(am >> 1) - 1];
                al[k] = (am & 1) ? 255 - v : v;
            }
        }
        for (i = 0; i < 4; i++) {
            int a = i < 3 ? rgb[0][i] : al[0], b = i < 3 ? rgb[1][i] : al[1], c = i < 3 ? rgb[2][i] : al[2];
            int op = i < 3 ? cop : aop, v;
            switch (op) {
            case 0: v = a; break;
            case 1: v = a * b / 255; break;
            case 2: v = a + b; break;
            case 3: v = a + b - 128; break;
            case 4: v = (a * c + b * (255 - c)) / 255; break;
            case 5: v = a - b; break;
            case 6: case 7:
                v = ((rgb[0][0] * 2 - 255) * (rgb[1][0] * 2 - 255) + 128) / 256 +
                    ((rgb[0][1] * 2 - 255) * (rgb[1][1] * 2 - 255) + 128) / 256 +
                    ((rgb[0][2] * 2 - 255) * (rgb[1][2] * 2 - 255) + 128) / 256;
                break;
            case 8: v = (a * b + 255 * c) / 255; break;
            case 9: v = (a + b > 255 ? 255 : a + b) * c / 255; break;
            default: v = a; break;
            }
            res[i] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        if (cop == 7)
            res[3] = res[0];
        for (i = 0; i < 3; i++)
            out[i] = res[i] * cmul > 255 ? 255 : res[i] * cmul;
        out[3] = res[3] * amul > 255 ? 255 : res[3] * amul;
        memcpy(buf, next, 4);
        if (upd & (1u << (8 + st)))
            memcpy(next, out, 3);
        if (upd & (1u << (12 + st)))
            next[3] = out[3];
    }
}

static int blend_factor(unsigned f, int ch, const uint8_t src[4], const uint8_t dst[4], const uint8_t k[4])
{
    switch (f) {
    case 0: return 0;
    case 1: return 255;
    case 2: return src[ch];
    case 3: return 255 - src[ch];
    case 4: return dst[ch];
    case 5: return 255 - dst[ch];
    case 6: return src[3];
    case 7: return 255 - src[3];
    case 8: return dst[3];
    case 9: return 255 - dst[3];
    case 10: return k[ch];
    case 11: return 255 - k[ch];
    case 12: return k[3];
    case 13: return 255 - k[3];
    default: return ch == 3 ? 255 : (src[3] < 255 - dst[3] ? src[3] : 255 - dst[3]);
    }
}

static void fragment(Target *t, int x, int y, float depth, const uint8_t prim[4], float uv[3][2])
{
    PicaSim *s = t->s;
    uint32_t fy = t->fbh - 1 - (uint32_t)y;     /* buffers are stored top row first */
    uint32_t dm = s->reg[0x107], at = s->reg[0x104];
    uint8_t tex[3][4], c[4], d[4], o[4];
    int i;
    memset(tex, 0, sizeof tex);
    for (i = 0; i < 3; i++)
        if (t->tu[i].on)
            tex_sample(&t->tu[i], uv[i][0], uv[i][1], tex[i]);
    tev(s, prim, tex, c);
    if ((at & 1) && !compare(at >> 4, c[3], (at >> 8) & 0xFF))
        return;
    if (t->depth) {
        uint8_t *dp = t->depth + tiled_off(x, fy, t->fbw, t->dbpp);
        uint32_t bits = t->dfmt == 0 ? 16 : 24, z = (uint32_t)(depth * (float)((1u << bits) - 1)), ref;
        ref = dp[0] | dp[1] << 8 | (bits == 24 ? dp[2] << 16 : 0);
        if ((dm & 1) && !compare(dm >> 4, z, ref))
            return;
        if ((dm & 0x1000) && (s->reg[0x115] & 3)) {
            dp[0] = z; dp[1] = z >> 8;
            if (bits == 24)
                dp[2] = z >> 16;
        }
    }
    if (!t->color || !(s->reg[0x113] & 0xF))
        return;
    {
        uint8_t *cp = t->color + tiled_off(x, fy, t->fbw, t->cbpp);
        px_decode(t->cfmt, cp, d);
        if (s->reg[0x100] & 0x100) {
            uint32_t bf = s->reg[0x101], bk = s->reg[0x103];
            uint8_t k[4] = { bk, bk >> 8, bk >> 16, bk >> 24 };
            for (i = 0; i < 4; i++) {
                unsigned eq = (i < 3 ? bf : bf >> 8) & 7;
                int sf = blend_factor((bf >> (i < 3 ? 16 : 24)) & 15, i, c, d, k);
                int df = blend_factor((bf >> (i < 3 ? 20 : 28)) & 15, i, c, d, k);
                int v;
                switch (eq) {
                case 1: v = (c[i] * sf - d[i] * df) / 255; break;
                case 2: v = (d[i] * df - c[i] * sf) / 255; break;
                case 3: v = c[i] < d[i] ? c[i] : d[i]; break;
                case 4: v = c[i] > d[i] ? c[i] : d[i]; break;
                default: v = (c[i] * sf + d[i] * df) / 255; break;
                }
                o[i] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
        } else {
            /* LOGIC_OP: 0 clear, 1 and, 2 and-reverse, 3 copy, 4 set, 5 copy-inverted, 6 noop,
               7 invert, 8 nand, 9 or, 10 nor, 11 xor, 12 equiv, 13 and-inverted, 14 or-reverse,
               15 or-inverted. Truth table bits: 8 = s&d, 4 = s&~d, 2 = ~s&d, 1 = ~s&~d. */
            static const uint8_t table[16] = { 0x0, 0x8, 0x4, 0xC, 0xF, 0x3, 0xA, 0x5, 0x7, 0xE, 0x1, 0x6, 0x9, 0x2, 0xD, 0xB };
            unsigned tb = table[s->reg[0x102] & 15];
            for (i = 0; i < 4; i++) {
                unsigned sv = c[i], dv = d[i], r = 0;
                if (tb & 8) r |= sv & dv;
                if (tb & 4) r |= sv & ~dv;
                if (tb & 2) r |= ~sv & dv;
                if (tb & 1) r |= ~sv & ~dv;
                o[i] = (uint8_t)r;
            }
        }
        for (i = 0; i < 4; i++)
            if (!(dm & (0x100u << i)))
                o[i] = d[i];
        px_encode(t->cfmt, cp, o);
        s->st.pixels++;
    }
}

static int edge(int ax, int ay, int bx, int by, int cx, int cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static int right_or_flat_bottom(const int v[2], const int a[2], const int b[2])
{
    if (a[1] == b[1])
        return v[1] < a[1];
    return v[0] < a[0] + (b[0] - a[0]) * (v[1] - a[1]) / (b[1] - a[1]);
}

static void screen_coords(PicaSim *s, RVtx *v)
{
    float iw = 1.0f / v->pos[3];
    int i;
    v->pos[3] = iw;
    for (i = 0; i < 4; i++)
        v->col[i] *= iw;
    for (i = 0; i < 3; i++) {
        v->tc[i][0] *= iw;
        v->tc[i][1] *= iw;
    }
    v->sx = (v->pos[0] * iw + 1.0f) * f24(s->reg[0x041]) + (float)(s->reg[0x068] & 0x3FF);
    v->sy = (v->pos[1] * iw + 1.0f) * f24(s->reg[0x043]) + (float)((s->reg[0x068] >> 16) & 0x3FF);
    v->sz = v->pos[2] * iw;
}

static void raster(Target *t, const RVtx *v0, const RVtx *v1, const RVtx *v2, int reversed)
{
    PicaSim *s = t->s;
    const RVtx *v[3] = { v0, v1, v2 };
    int p[3][2], i, x, y, minx, miny, maxx, maxy, bias[3];
    unsigned cull = s->reg[0x040] & 3, smode = s->reg[0x065] & 3;
    int sx1 = (s->reg[0x066] & 0x3FF) << 4, sy1 = ((s->reg[0x066] >> 16) & 0x3FF) << 4;
    int sx2 = ((s->reg[0x067] & 0x3FF) + 1) << 4, sy2 = (((s->reg[0x067] >> 16) & 0x3FF) + 1) << 4;
    float dscale = f24(s->reg[0x04D]), doff = f24(s->reg[0x04E]);

    for (i = 0; i < 3; i++) {
        p[i][0] = (int)lroundf(v[i]->sx * 16.0f);
        p[i][1] = (int)lroundf(v[i]->sy * 16.0f);
    }
    if (cull == 0 || cull == 3) {
        if (!reversed && edge(p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]) <= 0) {
            raster(t, v0, v2, v1, 1);
            return;
        }
    } else {
        if (!reversed && cull == 1) {
            raster(t, v0, v2, v1, 1);
            return;
        }
        if (edge(p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]) <= 0)
            return;
    }
    minx = maxx = p[0][0];
    miny = maxy = p[0][1];
    for (i = 1; i < 3; i++) {
        if (p[i][0] < minx) minx = p[i][0];
        if (p[i][0] > maxx) maxx = p[i][0];
        if (p[i][1] < miny) miny = p[i][1];
        if (p[i][1] > maxy) maxy = p[i][1];
    }
    if (smode == 3) {
        if (minx < sx1) minx = sx1;
        if (miny < sy1) miny = sy1;
        if (maxx > sx2) maxx = sx2;
        if (maxy > sy2) maxy = sy2;
    }
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > (int)t->fbw << 4) maxx = (int)t->fbw << 4;
    if (maxy > (int)t->fbh << 4) maxy = (int)t->fbh << 4;
    minx &= ~15;
    miny &= ~15;
    maxx = (maxx + 15) & ~15;
    maxy = (maxy + 15) & ~15;
    bias[0] = right_or_flat_bottom(p[0], p[1], p[2]) ? -1 : 0;
    bias[1] = right_or_flat_bottom(p[1], p[2], p[0]) ? -1 : 0;
    bias[2] = right_or_flat_bottom(p[2], p[0], p[1]) ? -1 : 0;
    s->st.triangles++;

    for (y = miny + 8; y < maxy; y += 16)
        for (x = minx + 8; x < maxx; x += 16) {
            int w0, w1, w2, k;
            float b0, b1, b2, iw, depth, uv[3][2];
            uint8_t prim[4];
            if (smode == 1 && x >= sx1 && x < sx2 && y >= sy1 && y < sy2)
                continue;
            w0 = bias[0] + edge(p[1][0], p[1][1], p[2][0], p[2][1], x, y);
            w1 = bias[1] + edge(p[2][0], p[2][1], p[0][0], p[0][1], x, y);
            w2 = bias[2] + edge(p[0][0], p[0][1], p[1][0], p[1][1], x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0 || w0 + w1 + w2 == 0)
                continue;
            b0 = (float)w0; b1 = (float)w1; b2 = (float)w2;
            iw = 1.0f / (v[0]->pos[3] * b0 + v[1]->pos[3] * b1 + v[2]->pos[3] * b2);
            depth = (v[0]->sz * b0 + v[1]->sz * b1 + v[2]->sz * b2) / (float)(w0 + w1 + w2) * dscale + doff;
            if (!(s->reg[0x06D] & 1))           /* W-buffer */
                depth *= iw * (float)(w0 + w1 + w2);
            depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
            for (k = 0; k < 4; k++) {
                float cv = (v[0]->col[k] * b0 + v[1]->col[k] * b1 + v[2]->col[k] * b2) * iw;
                prim[k] = (uint8_t)lroundf((cv < 0.0f ? 0.0f : cv > 1.0f ? 1.0f : cv) * 255.0f);
            }
            for (k = 0; k < 3; k++) {
                uv[k][0] = (v[0]->tc[k][0] * b0 + v[1]->tc[k][0] * b1 + v[2]->tc[k][0] * b2) * iw;
                uv[k][1] = (v[0]->tc[k][1] * b0 + v[1]->tc[k][1] * b1 + v[2]->tc[k][1] * b2) * iw;
            }
            fragment(t, x >> 4, y >> 4, depth, prim, uv);
        }
}

static void lerp_vtx(RVtx *o, float f, const RVtx *a, const RVtx *b)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)o;
    int i;
    for (i = 0; i < 14; i++)                    /* pos, col, tc */
        po[i] = pa[i] * f + pb[i] * (1.0f - f);
}

/* clip-space triangle -> clipped fan -> rasterizer */
static void triangle(Target *t, const RVtx *a, const RVtx *b, const RVtx *c)
{
    static const float planes[7][5] = {
        { -1, 0, 0, 1, 0 }, { 1, 0, 0, 1, 0 }, { 0, -1, 0, 1, 0 }, { 0, 1, 0, 1, 0 },
        { 0, 0, -1, 0, 0 }, { 0, 0, 1, 1, 0 }, { 0, 0, 0, 1, 0.00001f },
    };
    RVtx bufa[10], bufb[10], *in = bufb, *out = bufa, *tmp;
    int n = 3, m, e, i;
    out[0] = *a; out[1] = *b; out[2] = *c;
    for (e = 0; e < 7; e++) {
        const float *pl = planes[e];
        const RVtx *ref;
        float dref;
        tmp = in; in = out; out = tmp;
        m = 0;
        ref = &in[n - 1];
        dref = ref->pos[0] * pl[0] + ref->pos[1] * pl[1] + ref->pos[2] * pl[2] + (ref->pos[3] + pl[4]) * pl[3];
        for (i = 0; i < n; i++) {
            const RVtx *v = &in[i];
            float dv = v->pos[0] * pl[0] + v->pos[1] * pl[1] + v->pos[2] * pl[2] + (v->pos[3] + pl[4]) * pl[3];
            int vin = dv >= -0.000001f, rin = dref >= -0.000001f;
            if (vin != rin && m < 10)
                lerp_vtx(&out[m++], dref / (dref - dv), v, ref);
            if (vin && m < 10)
                out[m++] = *v;
            ref = v;
            dref = dv;
        }
        n = m;
        if (n < 3)
            return;
    }
    for (i = 0; i < n; i++)
        screen_coords(t->s, &out[i]);
    for (i = 0; i + 2 < n; i++)
        raster(t, &out[0], &out[i + 1], &out[i + 2], 0);
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static int bind_target(PicaSim *s, Target *t)
{
    static const uint16_t tb[3] = { 0x081, 0x091, 0x099 }, tf[3] = { 0x08E, 0x096, 0x09E };
    uint32_t dim = s->reg[0x11E], caddr = (s->reg[0x11D] & 0x0FFFFFFF) << 3, daddr = (s->reg[0x11C] & 0x0FFFFFFF) << 3;
    int i;
    memset(t, 0, sizeof *t);
    t->s = s;
    t->fbw = dim & 0x7FF;
    t->fbh = ((dim >> 12) & 0x3FF) + 1;
    t->cfmt = (s->reg[0x117] >> 16) & 7;
    if (t->cfmt == 2 || t->cfmt == 3)           /* colour buffers number RGB5A1/RGB565 the other way round than PPF */
        t->cfmt ^= 1;
    t->dfmt = s->reg[0x116] & 3;
    if (t->cfmt > 4 || !t->fbw) {
        sim_log(s, "picasim: bad render target (format %u, width %u)", t->cfmt, t->fbw);
        return 0;
    }
    t->cbpp = fb_bpp[t->cfmt];
    t->dbpp = t->dfmt == 0 ? 2 : t->dfmt == 2 ? 3 : 4;
    /* tiles: the stored image is padded to whole 8x8 blocks */
    if ((t->fbw | t->fbh) & 7) {
        sim_log(s, "picasim: render target %ux%u is not a multiple of 8", t->fbw, t->fbh);
        return 0;
    }
    if (caddr)
        t->color = mem(s, caddr, t->fbw * t->fbh * t->cbpp, 1);
    if (daddr && ((s->reg[0x114] | s->reg[0x115]) & 3))
        t->depth = mem(s, daddr, t->fbw * t->fbh * t->dbpp, 1);
    for (i = 0; i < 3; i++) {
        const uint32_t *r = &s->reg[tb[i]];
        TexUnit *u = &t->tu[i];
        uint32_t addr = (r[4] & 0x0FFFFFFF) << 3;
        if (!(s->reg[0x080] & (1u << i)))
            continue;
        u->on = 1;
        u->w = (r[1] >> 16) & 0x7FF;
        u->h = r[1] & 0x7FF;
        u->fmt = s->reg[tf[i]] & 15;
        u->linear = (r[2] >> 1) & 1;
        u->wrap_t = (r[2] >> 8) & 7;
        u->wrap_s = (r[2] >> 12) & 7;
        u->border[0] = r[0]; u->border[1] = r[0] >> 8; u->border[2] = r[0] >> 16; u->border[3] = r[0] >> 24;
        if (i == 0 && ((r[2] >> 28) & 7))
            sim_log(s, "picasim: texture unit 0 type %u not modelled%.0u", (r[2] >> 28) & 7, 0);
        if (u->fmt > 11 || !u->w || !u->h || !addr) {
            if (addr)
                sim_log(s, "picasim: texture format %u / size 0x%x not modelled", u->fmt, r[1]);
            continue;                           /* samples opaque black */
        }
        u->data = mem(s, addr, u->w * u->h * tex_bits[u->fmt] / 8, 0);
    }
    return 1;
}

static void draw(PicaSim *s, int indexed)
{
    static const uint8_t esize[4] = { 1, 1, 2, 4 };
    Target t;
    uint32_t fmt_lo = s->reg[0x201], fmt_hi = s->reg[0x202];
    uint32_t base = (s->reg[0x200] & 0x1FFFFFFE) << 3, count = s->reg[0x228], first = s->reg[0x22A];
    unsigned nattr = (fmt_hi >> 28) + 1, fixed_mask = (fmt_hi >> 16) & 0xFFF, mode = (s->reg[0x25E] >> 8) & 3;
    const uint8_t *src[12];
    unsigned stride[12];
    const uint8_t *idx = NULL;
    int idx16 = s->reg[0x227] >> 31, have[12];
    uint32_t i, maxv = 0;
    unsigned b, k, a;
    RVtx hist[2];
    unsigned nhist = 0, parity = 0;
    RVtx tri[3];
    unsigned ntri = 0;

    s->st.draws++;
    if (s->reg[0x229] & 3) {
        sim_log(s, "picasim: geometry shader stage not modelled (GEOSTAGE_CONFIG 0x%x)%.0u", s->reg[0x229], 0);
        return;
    }
    if (!count || count > 0x100000 || !bind_target(s, &t))
        return;
    if (indexed) {
        idx = mem(s, base + (s->reg[0x227] & 0x0FFFFFFF), count << idx16, 0);
        if (!idx)
            return;
        for (i = 0; i < count; i++) {
            uint32_t v = idx16 ? (uint32_t)(idx[2 * i] | idx[2 * i + 1] << 8) : idx[i];
            if (v > maxv) maxv = v;
        }
    } else {
        maxv = first + count - 1;
    }
    memset(have, 0, sizeof have);
    for (b = 0; b < 12; b++) {
        const uint32_t *r = &s->reg[0x203 + 3 * b];
        uint64_t comps = r[1] | (uint64_t)(r[2] & 0xFFFF) << 32;
        unsigned n = r[2] >> 28, st = (r[2] >> 16) & 0xFF, off = 0;
        const uint8_t *p;
        if (!n || !st)
            continue;
        p = mem(s, base + (r[0] & 0x0FFFFFFF), (maxv + 1) * st, 0);
        if (!p)
            return;
        for (k = 0; k < n; k++) {
            a = (comps >> (4 * k)) & 15;
            if (a < 12) {
                unsigned f = ((a < 8 ? fmt_lo >> (4 * a) : fmt_hi >> (4 * (a - 8)))) & 15, es = esize[f & 3];
                off = (off + es - 1) & ~(es - 1);
                src[a] = p + off;
                stride[a] = st;
                have[a] = 1;
                off += es * ((f >> 2) + 1);
            } else {
                off += (a - 11) * 4;
            }
        }
    }

    for (i = 0; i < count; i++) {
        uint32_t vi = indexed ? (idx16 ? (uint32_t)(idx[2 * i] | idx[2 * i + 1] << 8) : idx[i]) : first + i;
        ShaderRegs r;
        float sem[24];
        RVtx v;
        unsigned o, slot = 0;
        memset(r.in, 0, sizeof r.in);
        memset(r.out, 0, sizeof r.out);
        for (a = 0; a < nattr && a < 12; a++) {
            unsigned f = ((a < 8 ? fmt_lo >> (4 * a) : fmt_hi >> (4 * (a - 8)))) & 15, n = (f >> 2) + 1;
            unsigned reg = ((a < 8 ? s->reg[0x2BB] >> (4 * a) : s->reg[0x2BC] >> (4 * (a - 8)))) & 15;
            float val[4] = { 0, 0, 0, 1 };
            if ((fixed_mask >> a) & 1) {
                memcpy(val, s->fixed[a], sizeof val);
            } else if (have[a]) {
                const uint8_t *p = src[a] + (size_t)vi * stride[a];
                for (k = 0; k < n; k++)
                    switch (f & 3) {
                    case 0: val[k] = (float)(int8_t)p[k]; break;
                    case 1: val[k] = (float)p[k]; break;
                    case 2: val[k] = (float)(int16_t)(p[2 * k] | p[2 * k + 1] << 8); break;
                    default: memcpy(&val[k], p + 4 * k, 4); break;
                    }
            }
            memcpy(r.in[reg], val, sizeof val);
        }
        run_shader(s, &r);
        s->st.vertices++;
        memset(sem, 0, sizeof sem);
        for (o = 0; o < 16 && slot < 7; o++) {
            uint32_t map;
            if (!(s->reg[0x2BD] & (1u << o)))
                continue;
            map = s->reg[0x050 + slot++];
            for (k = 0; k < 4; k++) {
                unsigned id = (map >> (8 * k)) & 0x1F;
                if (id < 24)
                    sem[id] = r.out[o][k];
            }
        }
        memcpy(v.pos, &sem[0], 16);
        for (k = 0; k < 4; k++) {               /* colours are |c| saturated before interpolation */
            float cv = fabsf(sem[8 + k]);
            v.col[k] = cv > 1.0f ? 1.0f : cv;
        }
        v.tc[0][0] = sem[12]; v.tc[0][1] = sem[13];
        v.tc[1][0] = sem[14]; v.tc[1][1] = sem[15];
        v.tc[2][0] = sem[22]; v.tc[2][1] = sem[23];
        v.sx = v.sy = v.sz = 0;

        if (mode == 1 || mode == 2) {           /* strip, fan */
            if (nhist == 2) {
                if (mode == 1 && parity)
                    triangle(&t, &hist[1], &hist[0], &v);
                else
                    triangle(&t, &hist[0], &hist[1], &v);
                if (mode == 1) {
                    hist[0] = hist[1];
                    parity ^= 1;
                }
                hist[1] = v;
            } else {
                hist[nhist++] = v;
            }
        } else {
            tri[ntri++] = v;
            if (ntri == 3) {
                triangle(&t, &tri[0], &tri[1], &tri[2]);
                ntri = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* P3D registers and command lists                                     */
/* ------------------------------------------------------------------ */

static void unpack_f24x4(const uint32_t w[3], float out[4])
{
    out[0] = f24(w[2] & 0xFFFFFF);
    out[1] = f24((w[2] >> 24) | (w[1] & 0xFFFF) << 8);
    out[2] = f24((w[1] >> 16) | (w[0] & 0xFF) << 16);
    out[3] = f24(w[0] >> 8);
}

/* returns 1 when the list must stop (FINALIZE with autostop) */
static int p3d_write(PicaSim *s, uint32_t id, uint32_t val, uint32_t mask)
{
    static const uint32_t expand[16] = {
        0x00000000, 0x000000FF, 0x0000FF00, 0x0000FFFF, 0x00FF0000, 0x00FF00FF, 0x00FFFF00, 0x00FFFFFF,
        0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF, 0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF };
    uint32_t m = expand[mask & 15];
    if (id >= 0x300)
        return 0;
    s->reg[id] = (s->reg[id] & ~m) | (val & m);
    val = s->reg[id];
    if (id == 0x000) {                          /* IRQ acknowledge */
        s->gx[GX_IRQ_STAT] &= ~STAT_P3D;
    } else if (id == 0x010) {                   /* FINALIZE: any byte equal to IRQ_CMP's */
        uint32_t c = s->reg[0x020], x = ~(val ^ c);
        if ((x & 0xFF) == 0xFF || (x & 0xFF00) == 0xFF00 || (x & 0xFF0000) == 0xFF0000 || (x >> 24) == 0xFF) {
            s->gx[GX_IRQ_STAT] |= STAT_P3D;
            raise_irq(s, 45);
            return s->reg[0x034] & 1;
        }
    } else if (id == 0x22E || id == 0x22F) {
        draw(s, id == 0x22F);
    } else if (id == 0x232) {
        s->fixed_index = val & 15;
        s->fifo_n = 0;
        if (s->fixed_index == 15)
            sim_log(s, "picasim: immediate-mode vertex submission not modelled%.0u%.0u", 0, 0);
    } else if (id >= 0x233 && id <= 0x235) {
        s->fifo[s->fifo_n++] = val;
        if (s->fifo_n == 3) {
            if (s->fixed_index < 12)
                unpack_f24x4(s->fifo, s->fixed[s->fixed_index]);
            s->fifo_n = 0;
        }
    } else if (id == 0x2C0) {
        s->uni_index = val & 0xFF;
        s->uni_f32 = val >> 31;
        s->fifo_n = 0;
    } else if (id >= 0x2C1 && id <= 0x2C8) {
        s->fifo[s->fifo_n++] = val;
        if (s->fifo_n == (s->uni_f32 ? 4 : 3)) {
            float *u = s->uni[s->uni_index % 96];
            if (s->uni_f32) {
                memcpy(&u[3], &s->fifo[0], 4);
                memcpy(&u[2], &s->fifo[1], 4);
                memcpy(&u[1], &s->fifo[2], 4);
                memcpy(&u[0], &s->fifo[3], 4);
            } else {
                unpack_f24x4(s->fifo, u);
            }
            s->uni_index++;
            s->fifo_n = 0;
        }
    } else if (id == 0x2CB) {
        s->code_off = val & 0xFFF;
    } else if (id >= 0x2CC && id <= 0x2D3) {
        if (s->code_off < 512)
            s->code[s->code_off++] = val;
    } else if (id == 0x2D5) {
        s->opdesc_off = val & 0xFFF;
    } else if (id >= 0x2D6 && id <= 0x2DD) {
        if (s->opdesc_off < 128)
            s->opdesc[s->opdesc_off++] = val;
    }
    return 0;
}

static void run_list(PicaSim *s, uint32_t addr, uint32_t bytes)
{
    const uint8_t *p = mem(s, addr, bytes, 0);
    uint32_t n = bytes / 4, i = 0;
    s->st.lists++;
    if (!p)
        return;
    while (i + 1 < n) {
        uint32_t val, hdr, id, mask, extra, k;
        int seq;
        memcpy(&val, p + 4 * i, 4);
        memcpy(&hdr, p + 4 * i + 4, 4);
        i += 2;
        id = hdr & 0xFFFF;
        mask = (hdr >> 16) & 15;
        extra = (hdr >> 20) & 0xFF;
        seq = hdr >> 31;
        s->st.commands++;
        if (p3d_write(s, id, val, mask))
            return;
        for (k = 0; k < extra && i < n; k++, i++) {
            memcpy(&val, p + 4 * i, 4);
            if (p3d_write(s, seq ? id + 1 + k : id, val, mask))
                return;
        }
        i = (i + 1) & ~1u;
        if (id == 0x23C || id == 0x23D) {       /* jump to the other buffer */
            int j = id - 0x23C;
            run_list(s, (s->reg[0x23A + j] & 0x1FFFFFFF) << 3, (s->reg[0x238 + j] & 0x1FFFFF) << 3);
            return;
        }
    }
    sim_log(s, "picasim: command list at 0x%08x (%u bytes) ended without FINALIZE", addr, bytes);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

void picasim_init(PicaSim *s, const PicaSimHost *host)
{
    memset(s, 0, sizeof *s);
    s->host = *host;
}

uint32_t picasim_read(PicaSim *s, uint32_t off)
{
    off &= 0x1FFC;
    if (off >= 0x1000)
        return off < 0x1000 + 0x300 * 4 ? s->reg[(off - 0x1000) / 4] : 0;
    return s->gx[off / 4];
}

void picasim_write(PicaSim *s, uint32_t off, uint32_t val)
{
    off &= 0x1FFC;
    if (off >= 0x1000) {
        uint32_t id = (off - 0x1000) / 4;
        p3d_write(s, id, val, 0xF);
        if (id == 0x23C || id == 0x23D) {
            int j = id - 0x23C;
            run_list(s, (s->reg[0x23A + j] & 0x1FFFFFFF) << 3, (s->reg[0x238 + j] & 0x1FFFFF) << 3);
        }
        return;
    }
    if (off / 4 == GX_IRQ_STAT)
        return;                                 /* read only */
    s->gx[off / 4] = val;
    if (off == 0x01C || off == 0x02C) {         /* PSC fill control */
        int n = off == 0x02C;
        if (val & 1)
            psc_fill(s, n);
        else if (!(val & 2))
            s->gx[GX_IRQ_STAT] &= ~(STAT_PSC0 << n);
    } else if (off == 0xC18) {                  /* PPF control */
        if (val & 1)
            ppf_run(s);
        else if (!(val & 0x100))
            s->gx[GX_IRQ_STAT] &= ~STAT_PPF;
    }
}
