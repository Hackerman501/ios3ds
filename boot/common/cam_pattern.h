/*
 * ios3ds camera: the moving test pattern (QEMU's camera model, the plug-in's
 * pattern source and the host tests all draw the same picture) and the
 * YUV422 -> NV12 ('420f') conversion the plug-in does. Pure C, no libc.
 *
 * Pattern, YUV422 (word = Y0 | U << 8 | Y1 << 16 | V << 24, like the CAM FIFO):
 *   eight vertical colour bars that scroll left by 2 pixels per frame, a white
 *   square that bounces, and the camera number as 1 or 2 black blocks in the
 *   top left corner (outer = 1, inner = 2). Every frame differs, so a still
 *   photo of it identifies the frame and the camera.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* BT.601 full-range YUV of white, yellow, cyan, green, magenta, red, blue, black */
static const uint8_t cam_bars_yuv[8][3] = {
    { 255, 128, 128 }, { 226,   0, 149 }, { 179, 170,   0 }, { 150,  44,  21 },
    { 105, 212, 235 }, {  76,  85, 255 }, {  29, 255, 107 }, {   0, 128, 128 },
};

static inline uint32_t cam_yuyv(uint32_t y0, uint32_t y1, uint32_t u, uint32_t v)
{
    return y0 | u << 8 | y1 << 16 | v << 24;
}

/* one pixel pair (x even) of frame `n` from camera `cam` */
static inline uint32_t cam_pattern_word(uint32_t cam, uint32_t n, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t bw = w / 8, bx, bsx, bsy, period, t, s = h / 6;
    const uint8_t *c;
    /* camera marker: 1 or 2 black 16x16 blocks at y 8..23 */
    if (y >= 8 && y < 24) {
        for (uint32_t i = 0; i <= cam; i++)
            if (x >= 8 + i * 24 && x < 24 + i * 24) return cam_yuyv(16, 16, 128, 128);
    }
    /* bouncing white square */
    period = (w - s) * 2;
    t = (n * 6) % period;
    bsx = t < w - s ? t : period - t;
    period = (h - s) * 2;
    t = (n * 4) % period;
    bsy = t < h - s ? t : period - t;
    if (x >= bsx && x < bsx + s && y >= bsy && y < bsy + s) return cam_yuyv(235, 235, 128, 128);
    bx = ((x + n * 2) / bw) & 7;
    c = cam_bars_yuv[bx];
    return cam_yuyv(c[0], c[0], c[1], c[2]);
}

/* YUV422 -> 32-bit BGRA (kCVPixelFormatType_32BGRA: bytes B, G, R, A), BT.601
   full range. CoreAnimation composites BGRA IOSurfaces itself; 4:2:0 surfaces
   would go to an AppleM2CLCD overlay window, which ios3ds does not show
   (docs/camera.md section 4). */
static inline uint32_t cam_clamp8(int32_t v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

static inline uint32_t cam_bgra(int32_t y, int32_t u, int32_t v)
{
    int32_t r = y + ((91881 * v) >> 16);
    int32_t g = y - ((22554 * u + 46802 * v) >> 16);
    int32_t b = y + ((116130 * u) >> 16);
    return cam_clamp8(b) | cam_clamp8(g) << 8 | cam_clamp8(r) << 16 | 0xFF000000u;
}

static inline void cam_yuyv_to_bgra(const uint32_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t stride)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *s = src + y * (w / 2);
        uint32_t *d = (uint32_t *)(dst + y * stride);
        for (uint32_t i = 0; i < w / 2; i++) {
            uint32_t p = s[i];
            int32_t u = (int32_t)((p >> 8) & 0xFF) - 128, v = (int32_t)(p >> 24) - 128;
            d[2 * i] = cam_bgra((int32_t)(p & 0xFF), u, v);
            d[2 * i + 1] = cam_bgra((int32_t)((p >> 16) & 0xFF), u, v);
        }
    }
}

/* BGRA 2x upscale with linear interpolation (stills, see vcd.c still()) */
static inline uint32_t cam_avg4(uint32_t a, uint32_t b)
{
    return ((a >> 1) & 0x7F7F7F7Fu) + ((b >> 1) & 0x7F7F7F7Fu) + (a & b & 0x01010101u);
}

static inline void cam_bgra_upscale2x(const uint8_t *src, uint32_t sstride, uint32_t w, uint32_t h,
                                      uint8_t *dst, uint32_t dstride)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *a = (const uint32_t *)(src + y * sstride);
        const uint32_t *b = (const uint32_t *)(src + (y + 1 < h ? y + 1 : y) * sstride);
        uint32_t *d0 = (uint32_t *)(dst + 2 * y * dstride), *d1 = (uint32_t *)(dst + (2 * y + 1) * dstride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t xr = x + 1 < w ? x + 1 : x;
            uint32_t p = a[x], q = a[xr], r = cam_avg4(p, b[x]), t = cam_avg4(q, b[xr]);
            d0[2 * x] = p;
            d0[2 * x + 1] = cam_avg4(p, q);
            d1[2 * x] = r;
            d1[2 * x + 1] = cam_avg4(r, t);
        }
    }
}
