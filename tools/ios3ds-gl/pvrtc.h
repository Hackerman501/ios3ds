/* Software PVRTC decoder for the ios3ds GL engine.
 *
 * Converts PVRTC 2bpp/4bpp compressed images to the engine's 0xAARRGGBB
 * texels at glCompressedTexImage2D time. The four iOS format enums are
 * numbered consecutively, matching the four modes below.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IOS3DS_PVRTC_H
#define IOS3DS_PVRTC_H
#include <stdint.h>

enum {
    PVRTC_RGB_4BPP = 0,
    PVRTC_RGBA_4BPP,
    PVRTC_RGB_2BPP,
    PVRTC_RGBA_2BPP
};

/* src: w*h*8/16 bytes (one 64-bit block per 4x4 / 8x4 pixels); out: w*h
 * words. Returns 0 on a bad size, 1 on success. */
int pvrtc_decode(const uint8_t *src, int w, int h, int fmt, uint32_t *out);

/* The exact block layout, for the encoder in the host test:
 *   bits  0..31  modulation data (2 bits/pixel in 4bpp, 1 in 2bpp)
 *   bit   32     punch-through alpha flag (4bpp)
 *   bits 33..47  endpoint A, R5G5B5
 *   bit   48     endpoint A opaque flag
 *   bits 49..62  endpoint B, R5G5B4
 *   bit   63     endpoint B opaque flag
 *
 * In 2bpp there are no alpha flags and endpoint B uses all 15 bits at
 * 49..63 (R5G5B5); bit 48 is unused. pvrtc_block() takes the 4bpp layout;
 * the test encoder builds 2bpp blocks directly. */
uint64_t pvrtc_block(uint32_t mod, int punch, int a, int a_opaque, int b, int b_opaque);

#endif
