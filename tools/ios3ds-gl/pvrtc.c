/* Software PVRTC 2bpp/4bpp decoder (see pvrtc.h and docs/gpu.md).
 *
 * PVRTC is not a block codec like S3TC: two low-resolution colour signals
 * are bilinearly expanded and a low-precision modulation signal selects or
 * blends them (Fenney, "Texture Compression using Low-Frequency Signal
 * Modulation", Graphics Hardware 2003; the 64-bit word layout is the one
 * documented for PVRTC in the Khronos/Imagination format descriptions).
 *
 * We decode on upload, so there is no GPU-side PVRTC support to emulate:
 * the texture becomes ordinary RGBA texels. 4bpp uses the block's two
 * endpoints and its 2-bit per-pixel modulation exactly, including punch-
 * through alpha. 2bpp has only one modulation bit per pixel; the missing
 * low-frequency signal is approximated by averaging the endpoints of the
 * neighbouring 8x4 blocks. Constant and gently varying iOS artwork decodes
 * correctly; this is not a bit-exact PowerVR reference decoder.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include "pvrtc.h"

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static unsigned expand5(unsigned v) { v &= 31; return (v << 3) | (v >> 2); }
static unsigned expand4(unsigned v) { v &= 15; return (v << 4) | v; }

/* Endpoint A: R5G5B5. Endpoint B: R5G5B4 in 4bpp, R5G5B5 in 2bpp. */
static void endpoint(unsigned v, int b4, unsigned c[3])
{
    if (b4) {
        c[0] = expand5((v >> 9) & 31);
        c[1] = expand5((v >> 4) & 31);
        c[2] = expand4(v & 15);
    } else {
        c[0] = expand5((v >> 10) & 31);
        c[1] = expand5((v >> 5) & 31);
        c[2] = expand5(v & 31);
    }
}

static uint32_t px(const unsigned c[3], unsigned a)
{
    return (a << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
}

/* The four 2bpp blocks around (bx, by) supply the low-frequency signal.
   The image is always a power of two, so wrapping the block grid is right. */
static void neighbours(const uint8_t *src, int nbx, int nby, int bx, int by,
                       unsigned a[3], unsigned b[3])
{
    int i, j, k;
    unsigned ca[3], cb[3];
    a[0] = a[1] = a[2] = b[0] = b[1] = b[2] = 0;
    for (j = 0; j < 2; j++)
        for (i = 0; i < 2; i++) {
            int x = (bx - 1 + i + nbx) % nbx, y = (by - 1 + j + nby) % nby;
            uint64_t w = rd64(src + (size_t)(y * nbx + x) * 8);
            endpoint((unsigned)((w >> 33) & 0x7fff), 0, ca);
            endpoint((unsigned)((w >> 49) & 0x7fff), 0, cb);
            for (k = 0; k < 3; k++) {
                a[k] += ca[k];
                b[k] += cb[k];
            }
        }
    for (k = 0; k < 3; k++) {
        a[k] /= 4;
        b[k] /= 4;
    }
}

uint64_t pvrtc_block(uint32_t mod, int punch, int a, int a_opaque, int b, int b_opaque)
{
    return (uint64_t)mod | ((uint64_t)(punch & 1) << 32) |
           ((uint64_t)(a & 0x7fff) << 33) | ((uint64_t)(a_opaque & 1) << 48) |
           ((uint64_t)(b & 0x3fff) << 49) | ((uint64_t)(b_opaque & 1) << 63);
}

int pvrtc_decode(const uint8_t *src, int w, int h, int fmt, uint32_t *out)
{
    int b4 = fmt == PVRTC_RGB_4BPP || fmt == PVRTC_RGBA_4BPP;
    int alpha = fmt == PVRTC_RGBA_4BPP || fmt == PVRTC_RGBA_2BPP;
    int bw = b4 ? 4 : 8, bh = 4;
    int nbx, nby, bx, by, x, y;
    if (!src || !out || w < 8 || h < 8 || w % bw || h % bh || fmt < 0 || fmt > PVRTC_RGBA_2BPP)
        return 0;
    nbx = w / bw;
    nby = h / bh;
    for (by = 0; by < nby; by++)
        for (bx = 0; bx < nbx; bx++) {
            const uint8_t *s = src + (size_t)(by * nbx + bx) * 8;
            uint64_t word = rd64(s);
            uint32_t mod = (uint32_t)word;
            unsigned ca[3], cb[3], la[3], lb[3], c[3];
            unsigned a2 = (unsigned)((word >> 33) & 0x7fff);
            unsigned b2 = (unsigned)((word >> 49) & (b4 ? 0x3fffu : 0x7fffu));
            int punch = b4 && (word >> 32) & 1;
            int ao = b4 && (word >> 48) & 1, bo = b4 && (word >> 63) & 1;
            if (b4) {
                endpoint(a2, 0, ca);        /* A is R5G5B5 */
                endpoint(b2, 1, cb);        /* B is R5G5B4 */
            } else {
                endpoint(a2, 0, ca);
                endpoint(b2, 0, cb);
                neighbours(src, nbx, nby, bx, by, la, lb);
            }
            for (y = 0; y < bh; y++)
                for (x = 0; x < bw; x++) {
                    int idx = y * bw + x, m = (int)((mod >> (b4 ? 2 * idx : idx)) & (b4 ? 3u : 1u));
                    unsigned wt = b4 ? (m == 0 ? 0 : m == 3 ? 255 : m == 1 ? 85 : 170) : (m ? 255u : 0u);
                    unsigned al = 255;
                    int k;
                    if (b4 && punch && m == 2) {
                        al = 0;                         /* punch-through: endpoint A, alpha 0 */
                        wt = 0;
                    }
                    else if (alpha && !b4)
                        al = wt;                        /* 2bpp: one bit per pixel */
                    else if (alpha && !(ao && bo)) {
                        unsigned aa = ao ? 255 : 0, ab = bo ? 255 : 0;
                        al = (aa * (255 - wt) + ab * wt) / 255;
                    }
                    if (b4) {
                        for (k = 0; k < 3; k++)
                            c[k] = (ca[k] * (255 - wt) + cb[k] * wt) / 255;
                    } else {
                        for (k = 0; k < 3; k++) {
                            unsigned lo = (ca[k] + la[k]) / 2, hi = (cb[k] + lb[k]) / 2;
                            c[k] = (lo * (255 - wt) + hi * wt) / 255;
                        }
                    }
                    out[(size_t)(by * bh + y) * w + bx * bw + x] = px(c, alpha ? al : 255);
                }
        }
    return 1;
}
