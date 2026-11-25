/* Host unit test for the PVRTC decoder: hand-encoded blocks, no Apple files.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../pvrtc.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static unsigned red5(void)  { return 31 << 10; }
static unsigned blue5(void) { return 31; }
static unsigned red4(void)  { return 31 << 9; }         /* 4bpp B: R5G5B4 */
static unsigned blue4(void) { return 15; }

int main(void)
{
    uint8_t src[8 * 8 * 8];                /* 8x8: four 4bpp or two 2bpp blocks */
    uint32_t px[8 * 8];
    memset(src, 0, sizeof src);

    /* RGB 4bpp: both endpoints red, any modulation => opaque red everywhere */
    for (int b = 0; b < 4; b++)
        put64(src + b * 8, pvrtc_block(0x1B5A5A5Bu, 0, red5(), 1, red4(), 1));
    CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGB_4BPP, px) == 1, "4bpp RGB decode");
    for (int i = 0; i < 64; i++)
        CHECK(px[i] == 0xFFFF0000u, "4bpp RGB block %d texel %d: %08x", i / 16, i, px[i]);
    printf("PVRTC 4bpp constant red: %s\n", fails ? "FAIL" : "ok");

    /* RGBA 4bpp: A=red (opaque), B=blue (opaque). Modulation 0 -> red,
       3 -> blue, 2 with punch-through -> transparent. */
    for (int b = 0; b < 8; b++)
        put64(src + b * 8, pvrtc_block(0u, 1, red5(), 1, blue4(), 1));
    CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGBA_4BPP, px) == 1, "4bpp RGBA decode");
    CHECK(px[0] == 0xFFFF0000u, "4bpp RGBA mod 0: %08x", px[0]);
    for (int b = 0; b < 8; b++)
        put64(src + b * 8, pvrtc_block(0xFFFFFFFFu, 1, red5(), 1, blue4(), 1));
    CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGBA_4BPP, px) == 1, "4bpp RGBA decode mod 3");
    CHECK(px[8 + 7] == 0xFF0000FFu, "4bpp RGBA mod 3: %08x", px[8 + 7]);
    for (int b = 0; b < 8; b++)
        put64(src + b * 8, pvrtc_block(0xAAAAAAAAu, 1, red5(), 1, blue4(), 1));
    CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGBA_4BPP, px) == 1, "4bpp RGBA decode punch");
    CHECK(px[16 + 0] == 0x00FF0000u, "4bpp RGBA punch: %08x", px[16]);
    printf("PVRTC 4bpp RGBA endpoints and punch-through: %s\n", fails ? "FAIL" : "ok");

    /* 2bpp: one modulation bit per pixel, endpoints A/B both blue in every
       block, so the low-frequency interpolation cannot change the colour. */
    {
        int before = fails;
        for (int b = 0; b < 2; b++)
            put64(src + b * 8, (uint64_t)0x5A5A5A5Au | ((uint64_t)blue5() << 33) | ((uint64_t)blue5() << 49));
        CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGB_2BPP, px) == 1, "2bpp RGB decode");
        for (int i = 0; i < 64; i++)
            CHECK(px[i] == 0xFF0000FFu, "2bpp RGB texel %d: %08x", i, px[i]);
        printf("PVRTC 2bpp constant blue: %s\n", fails > before ? "FAIL" : "ok");
    }

    CHECK(pvrtc_decode(src, 12, 8, PVRTC_RGB_2BPP, px) == 0, "2bpp width not a multiple of 8");
    CHECK(pvrtc_decode(src, 8, 8, PVRTC_RGB_4BPP, NULL) == 0, "null output");
    printf("PVRTC unit test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
