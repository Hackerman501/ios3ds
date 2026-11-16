/*
 * Mirror iOS's framebuffer onto the 3DS top screen.
 * AppleM2CLCD (running against a fake S5L8720 CLCD register page) keeps the
 * scan-out state in that page: +0x00 bit0 = on, +0x24 = window-1 base PA,
 * +0x28 = width in pixels (stride = x4), +0x30 = (w<<16)|h, 32 bpp BGRA.
 * The 320x480 portrait image is rotated and scaled by 3/4 to 360x240 so it
 * fills the top LCD's height (turn the 3DS 90 degrees clockwise, like a book).
 * The bottom screen can show it too, at 2/3 scale: exactly the panel -> iOS
 * mapping of the input daemon (tools/ios3ds-input map_touch, flipx), so a
 * tap lands on what is drawn under the finger. Modes: display.h SCREEN_*.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "display.h"
#include "util.h"

#define SCR_W 400
#define SCR_H 240

static u8 *top_fb[2], *bot_fb[2];
static int mode = SCREEN_BOTH;
static u32 snapshot[0x100];
static bool armed, touched;

/* remember the register page as loaded; the mirror starts once the kernel
   driver has written to it (i.e. AppleM2CLCD is really driving "the panel") */
void display_arm(u32 regs_pa)
{
    if (!regs_pa) return;
    for (u32 i = 0; i < 0x100; i++) snapshot[i] = ((vu32 *)regs_pa)[i];
    armed = true;
}

void display_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1)
{
    top_fb[0] = top0;
    top_fb[1] = top1;
    bot_fb[0] = bot0;
    bot_fb[1] = bot1 != bot0 ? bot1 : NULL;
}

void display_set_mode(int m) { mode = m; }

/* bottom LCD (320x240): column c <- iOS row c*3/2, memory row rr (from the
   bottom) <- iOS column (3*rr-40)/2; 13 px black margins top and bottom */
static void blit_bottom(const u8 *src, u32 w, u32 h, u32 stride)
{
    for (int b = 0; b < 2; b++) {
        u8 *fb = bot_fb[b];
        if (!fb) continue;
        for (u32 c = 0; c < 320; c++) {
            u32 iy = c * 3 / 2;
            const u8 *row = src + (iy < h ? iy : h - 1) * stride;
            u8 *col = fb + c * SCR_H * 3;
            for (u32 rr = 0; rr < SCR_H; rr++) {
                u32 t = rr * 3;
                u8 *d = col + rr * 3;
                if (t < 40 || (t - 40) / 2 >= w) {
                    d[0] = d[1] = d[2] = 0;
                    continue;
                }
                const u8 *p = row + (t - 40) / 2 * 4;
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
            }
        }
    }
}

bool display_active(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    u32 base = r[0x24 / 4];
    if (!armed) return false;
    if (!touched) {
        for (u32 i = 0; i < 0x100 && !touched; i++) touched = r[i] != snapshot[i];
        if (!touched) return false;
    }
    return regs_pa && (r[0] & 1) && base >= VRAM_BASE && base < VRAM_BASE + 0x600000 && top_fb[0];
}

void display_blit(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    const u8 *src = (const u8 *)r[0x24 / 4];
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
    u32 stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride) return;
    if (mode != SCREEN_TOP) blit_bottom(src, w, h, stride);
    if (mode == SCREEN_BOTTOM) return;
    /* screen column sx (0..399) <- source row iy; screen row from top <- source column ix */
    u32 outw = h * 3 / 4;                     /* 360 for h = 480 */
    if (outw > SCR_W) outw = SCR_W;
    u32 x0 = (SCR_W - outw) / 2;
    for (int b = 0; b < 2; b++) {
        u8 *fb = top_fb[b];
        if (!fb) continue;
        for (u32 c = 0; c < outw; c++) {
            u32 iy = c * 4 / 3;
            const u8 *row = src + iy * stride;
            u8 *col = fb + (x0 + c) * SCR_H * 3;
            for (u32 rr = 0; rr < SCR_H; rr++) {
                /* memory goes bottom-to-top: source column 0 at the bottom
                   (rotate the 3DS 90 deg clockwise to read it upright) */
                u32 ix = rr * 4 / 3;
                if (ix >= w) ix = w - 1;
                const u8 *p = row + ix * 4;
                col[rr * 3 + 0] = p[0];
                col[rr * 3 + 1] = p[1];
                col[rr * 3 + 2] = p[2];
            }
        }
    }
}
