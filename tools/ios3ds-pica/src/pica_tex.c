/*
 * ios3ds-pica: texel conversion and tiling.
 *
 * The GPU stores textures and colour/depth buffers as 8x8 tiles, the tiles
 * in row-major order and the 64 texels inside a tile in Morton (Z) order
 * with x in the even bits. Row 0 of the stored image is the row sampled at
 * t = 1 (the top in GL terms), so GL's rows go in reversed.
 * Multi-byte texels are little-endian with the first-named channel in the
 * most significant bits, which makes RGBA8 "A, B, G, R" in memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "pica.h"

uint32_t pica_tex_bpp(unsigned fmt)
{
    static const uint8_t bpp[] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };
    return fmt < sizeof bpp ? bpp[fmt] : 0;
}

uint32_t pica_tex_bytes(unsigned fmt, uint32_t w, uint32_t h)
{
    return w * h * pica_tex_bpp(fmt) / 8;
}

uint32_t pica_pot(uint32_t n)
{
    uint32_t p = 8;
    while (p < n)
        p <<= 1;
    return p;
}

uint32_t pica_tex_index(uint32_t x, uint32_t y, uint32_t w)
{
    /* spread the low 3 bits of x and y: x -> bits 0,2,4  y -> bits 1,3,5 */
    static const uint8_t spread[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + spread[x & 7] + (spread[y & 7] << 1);
}

unsigned pica_tex_fmt_for(pica_srcfmt_t sfmt)
{
    switch (sfmt) {
    case PICA_SRC_RGBA8888:
    case PICA_SRC_BGRA8888: return PICA_TEX_RGBA8;
    case PICA_SRC_RGB888:   return PICA_TEX_RGB8;
    case PICA_SRC_RGB565:   return PICA_TEX_RGB565;
    case PICA_SRC_RGBA4444: return PICA_TEX_RGBA4;
    case PICA_SRC_RGBA5551: return PICA_TEX_RGBA5551;
    case PICA_SRC_L8:       return PICA_TEX_L8;
    case PICA_SRC_A8:       return PICA_TEX_A8;
    case PICA_SRC_LA88:     return PICA_TEX_LA8;
    }
    return PICA_TEX_RGBA8;
}

typedef struct { uint8_t r, g, b, a; } rgba_t;

static unsigned src_bytes(pica_srcfmt_t f)
{
    switch (f) {
    case PICA_SRC_RGBA8888: case PICA_SRC_BGRA8888: return 4;
    case PICA_SRC_RGB888: return 3;
    case PICA_SRC_RGB565: case PICA_SRC_RGBA4444: case PICA_SRC_RGBA5551: case PICA_SRC_LA88: return 2;
    case PICA_SRC_L8: case PICA_SRC_A8: return 1;
    }
    return 0;
}

static inline uint8_t ex5(unsigned v) { return (uint8_t)(v << 3 | v >> 2); }
static inline uint8_t ex6(unsigned v) { return (uint8_t)(v << 2 | v >> 4); }
static inline uint8_t ex4(unsigned v) { return (uint8_t)(v << 4 | v); }

static inline rgba_t load(const uint8_t *p, pica_srcfmt_t f)
{
    rgba_t c = { 0, 0, 0, 255 };
    unsigned v;

    switch (f) {
    case PICA_SRC_RGBA8888: c.r = p[0]; c.g = p[1]; c.b = p[2]; c.a = p[3]; break;
    case PICA_SRC_BGRA8888: c.b = p[0]; c.g = p[1]; c.r = p[2]; c.a = p[3]; break;
    case PICA_SRC_RGB888:   c.r = p[0]; c.g = p[1]; c.b = p[2]; break;
    case PICA_SRC_RGB565:
        v = p[0] | p[1] << 8;
        c.r = ex5(v >> 11); c.g = ex6((v >> 5) & 63); c.b = ex5(v & 31);
        break;
    case PICA_SRC_RGBA4444:
        v = p[0] | p[1] << 8;
        c.r = ex4(v >> 12); c.g = ex4((v >> 8) & 15); c.b = ex4((v >> 4) & 15); c.a = ex4(v & 15);
        break;
    case PICA_SRC_RGBA5551:
        v = p[0] | p[1] << 8;
        c.r = ex5(v >> 11); c.g = ex5((v >> 6) & 31); c.b = ex5((v >> 1) & 31); c.a = (v & 1) ? 255 : 0;
        break;
    case PICA_SRC_L8:   c.r = c.g = c.b = p[0]; break;
    case PICA_SRC_A8:   c.r = c.g = c.b = 0; c.a = p[0]; break;
    case PICA_SRC_LA88: c.r = c.g = c.b = p[0]; c.a = p[1]; break;
    }
    return c;
}

/* Rec. 601 luma, for RGB sources going into luminance textures. Exact for greys. */
static inline uint8_t luma(rgba_t c)
{
    return (uint8_t)((c.r * 77 + c.g * 150 + c.b * 29 + 128) >> 8);
}

/* Store texel number idx of a texture of format fmt. */
static inline void store(uint8_t *dst, unsigned fmt, uint32_t idx, rgba_t c)
{
    uint8_t *p;
    unsigned v;

    switch (fmt) {
    case PICA_TEX_RGBA8:
        p = dst + idx * 4; p[0] = c.a; p[1] = c.b; p[2] = c.g; p[3] = c.r;
        break;
    case PICA_TEX_RGB8:
        p = dst + idx * 3; p[0] = c.b; p[1] = c.g; p[2] = c.r;
        break;
    case PICA_TEX_RGBA5551:
        v = (c.r >> 3) << 11 | (c.g >> 3) << 6 | (c.b >> 3) << 1 | (c.a >> 7);
        p = dst + idx * 2; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        break;
    case PICA_TEX_RGB565:
        v = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3);
        p = dst + idx * 2; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        break;
    case PICA_TEX_RGBA4:
        v = (c.r >> 4) << 12 | (c.g >> 4) << 8 | (c.b >> 4) << 4 | (c.a >> 4);
        p = dst + idx * 2; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        break;
    case PICA_TEX_LA8:
        p = dst + idx * 2; p[0] = c.a; p[1] = luma(c);
        break;
    case PICA_TEX_L8:  dst[idx] = luma(c); break;
    case PICA_TEX_A8:  dst[idx] = c.a; break;
    case PICA_TEX_LA4: dst[idx] = (uint8_t)((luma(c) & 0xF0) | c.a >> 4); break;
    case PICA_TEX_L4:
    case PICA_TEX_A4:
        v = (fmt == PICA_TEX_L4 ? luma(c) : c.a) >> 4;
        p = dst + idx / 2;
        *p = (idx & 1) ? (uint8_t)((*p & 0x0F) | v << 4) : (uint8_t)((*p & 0xF0) | v);
        break;
    }
}

static int storable(unsigned fmt)
{
    return fmt <= PICA_TEX_A4 && fmt != PICA_TEX_HILO8;
}

int pica_tex_upload(void *dst, unsigned fmt, uint32_t dw, uint32_t dh,
                    uint32_t dx, uint32_t dy,
                    const void *src, pica_srcfmt_t sfmt, uint32_t sw, uint32_t sh, uint32_t stride)
{
    unsigned sb = src_bytes(sfmt);

    if (!storable(fmt) || !sb || (dw & (dw - 1)) || (dh & (dh - 1)) || dw < 8 || dh < 8)
        return -1;
    if (dx + sw > dw || dy + sh > dh || dx + sw < dx || dy + sh < dy)
        return -1;

    for (uint32_t y = 0; y < sh; y++) {
        const uint8_t *row = (const uint8_t *)src + (size_t)y * stride;
        uint32_t ty = dh - 1 - (dy + y);        /* GL row -> stored row */

        if (fmt == PICA_TEX_RGBA8 && sfmt == PICA_SRC_BGRA8888 && !((uintptr_t)row & 3) && !((uintptr_t)dst & 3)) {
            /* the common case (CoreGraphics / IOSurface pixels) without the
               detour: source bytes B,G,R,A become the RGBA8 texel bytes
               A,B,G,R, i.e. one 32-bit load, rotate left by 8, one 32-bit
               store. Both src and dst are 32-bit aligned (dst is the tiled
               scratch buffer, texel offsets are multiples of 4 bytes). */
            for (uint32_t x = 0; x < sw; x++) {
                uint32_t v = ((const uint32_t *)row)[x];
                *(uint32_t *)((uint8_t *)dst + pica_tex_index(dx + x, ty, dw) * 4) = (v << 8) | (v >> 24);
            }
            continue;
        }
        for (uint32_t x = 0; x < sw; x++, row += sb)
            store(dst, fmt, pica_tex_index(dx + x, ty, dw), load(row, sfmt));
    }
    return 0;
}

/* Copy one texel inside a texture (any storable format). */
static void copy_texel(uint8_t *dst, unsigned fmt, uint32_t to, uint32_t from)
{
    unsigned bits = pica_tex_bpp(fmt);

    if (bits == 4) {
        unsigned v = (dst[from / 2] >> ((from & 1) * 4)) & 15;
        uint8_t *p = dst + to / 2;
        *p = (to & 1) ? (uint8_t)((*p & 0x0F) | v << 4) : (uint8_t)((*p & 0xF0) | v);
    } else {
        memmove(dst + to * (bits / 8), dst + from * (bits / 8), bits / 8);
    }
}

void pica_tex_pad_edges(void *dst, unsigned fmt, uint32_t dw, uint32_t dh, uint32_t w, uint32_t h)
{
    uint8_t *d = dst;

    if (!storable(fmt) || !w || !h || w > dw || h > dh)
        return;
    /* content occupies GL texels x < w, y < h, i.e. stored rows dh-h .. dh-1 */
    if (w < dw)
        for (uint32_t y = 0; y < h; y++)
            copy_texel(d, fmt, pica_tex_index(w, dh - 1 - y, dw), pica_tex_index(w - 1, dh - 1 - y, dw));
    if (h < dh)
        for (uint32_t x = 0; x < (w < dw ? w + 1 : w); x++)
            copy_texel(d, fmt, pica_tex_index(x, dh - 1 - h, dw), pica_tex_index(x, dh - h, dw));
}

void pica_untile_rgba8_to_bgra(void *dst, uint32_t dst_stride,
                               const void *fb, uint32_t fb_w, uint32_t fb_h,
                               uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* GL window (x, y, w, h); output row 0 is the top GL row y + h - 1,
       which is stored row fb_h - 1 - (y + h - 1). */
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *o = (uint8_t *)dst + (size_t)r * dst_stride;
        uint32_t sy = fb_h - (y + h) + r;

        for (uint32_t i = 0; i < w; i++, o += 4) {
            const uint8_t *p = (const uint8_t *)fb + pica_tex_index(x + i, sy, fb_w) * 4;
            o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0];
        }
    }
}
