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
#include "log.h"
#include "blksrv.h"
#include "gpumirror.h"
#include "stereo.h"

#define SCR_W 400
#define SCR_H 240

static u8 *top_fb[2], *bot_fb[2];
static int mode = SCREEN_BOTH;
static u32 snapshot[0x100];
static bool armed, touched;

/* Experimental stereo copy (docs/stereo3d.md): boot.cfg stereo3d and the
   current 3D slider. With the flag off this code is dormant and the normal
   copy below runs exactly as before. */
static bool stereo_cfg, stereo_hw, stereo_no_shim, stereo_warned;
static u32 stereo_slider;

static void stereo_barrier(bool on);

void display_set_stereo(bool on, u32 slider)
{
    stereo_cfg = on && top_fb[0] && top_fb[1] && top_fb[0] != top_fb[1];
    stereo_slider = slider;
    if (!on || !stereo_cfg || slider < STEREO_MIN_SLIDER)
        stereo_no_shim = false;                 /* retry a failed switch next time */
    if (stereo_hw && (!stereo_cfg || !stereo_active(1, slider))) {
        stereo_barrier(false);
        gm_set_stereo(false);
        stereo_hw = false;
        logp("[monitor] stereo 3D: back to 2D, parallax barrier off\n");
    }
}

/* 3dbrew "LCD Registers": parallax barrier PWM 0x10202004 = 0x0A390A39 and
   enable 0x10202000 = 0x00010001 (libn3ds LCD_setParallaxBarrier); 0 = off.
   Written only after the ARM11 has acknowledged the PDC0 2D/3D switch. */
static void stereo_barrier(bool on)
{
    vu32 *cnt = (vu32 *)(uintptr_t)ST_BARRIER_PA;
    if (on) {
        *(vu32 *)(uintptr_t)ST_BARRIER_PWM_PA = ST_BARRIER_PWM;
        *cnt = ST_BARRIER_ON;
    } else {
        *cnt = 0;
    }
}

/* The stamp is an alpha-only GL pass. The iOS surface is 32 bpp BGRA, but
   the PICA present path's byte order has been described both ways (docs/gpu.md
   says B,G,R,A in memory, the original QEMU model used A,B,G,R), so accept
   the class magic in either end of the word. Colour bytes are masked out
   when the eye images are packed; the stamp never reaches the LCD. */
static int stereo_px_class(u32 px)
{
    int cls = stereo_class_of((u8)(px >> 24));
    return cls >= 0 ? cls : stereo_class_of((u8)px);
}

/* Sample the alpha plane of the iOS frame. The GL engine stamps a depth
   class into it on home frames only; every other frame (apps, lock screen,
   software compositor) has no class map and must stay flat. */
static bool stereo_is_home(const u8 *src, u32 w, u32 h, u32 stride)
{
    u8 a[1024];
    const u32 *px;
    if (w < 32 || h < 32 || stride < w * 4 || ((u32)(uintptr_t)src & 3))
        return false;
    px = (const u32 *)src;
    for (u32 j = 0; j < 32; j++) {
        u32 y = j * (h / 32);
        if (y >= h) y = h - 1;
        for (u32 i = 0; i < 32; i++) {
            u32 x = i * (w / 32);
            if (x >= w) x = w - 1;
            u32 v = px[y * (stride / 4) + x];
            int cls = stereo_px_class(v);
            a[j * 32 + i] = cls < 0 ? 0xFF : stereo_magic(cls);
        }
    }
    return stereo_alphas_stereo(a, (int)sizeof a) != 0;
}

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

/* A screen that starts showing iOS is cleared first: the copy never writes
   the top screen's 20-column side margins, which would otherwise keep
   whatever the console drew there. The GPU mirror (gpu_mirror.h) writes those
   margins black every frame; this keeps the CPU copy pixel-identical to it. */
void display_set_mode(int m)
{
    static int shown = -1;
    bool top_was = shown >= 0 && shown != SCREEN_BOTTOM, bot_was = shown >= 0 && shown != SCREEN_TOP;
    for (int i = 0; i < 2; i++) {
        if (m != SCREEN_BOTTOM && !top_was && top_fb[i]) memset(top_fb[i], 0, SCR_W * SCR_H * 3);
        if (m != SCREEN_TOP && !bot_was && bot_fb[i]) memset(bot_fb[i], 0, 320 * SCR_H * 3);
    }
    shown = mode = m;
}

/* the LCD framebuffers as Luma set them up: top 0/1, bottom 0/1 (0 if absent) */
void display_fbs(u32 out[4])
{
    out[0] = (u32)(uintptr_t)top_fb[0];
    out[1] = (u32)(uintptr_t)(top_fb[1] ? top_fb[1] : top_fb[0]);
    out[2] = (u32)(uintptr_t)bot_fb[0];
    out[3] = (u32)(uintptr_t)(bot_fb[1] ? bot_fb[1] : bot_fb[0]);
}

/* The old copy, kept for geometries the fast one does not take.
   Cost matters: the ARM11 kernel spins on every md0 request until this ARM9
   serves it, so the copy (a) checks the block mailbox every few columns,
   (b) reads each source pixel once as a word (it sits in VRAM), (c) uses
   precomputed column offsets, and (d) fills the second framebuffer of a
   pair with memcpy instead of re-reading the source. */
#define POLL_EVERY 8

/* md0 requests, and (display_set_poll) the buttons and touch screen: a CPU
   copy takes 150-430 ms on hardware and must not starve either (run 17) */
static void (*poll_extra)(void);
void display_set_poll(void (*fn)(void)) { poll_extra = fn; }
static void poll_io(void)
{
    blksrv_poll();
    if (poll_extra) poll_extra();
}

/* bottom LCD (320x240): column c <- iOS row c*3/2, memory row rr (from the
   bottom) <- iOS column (3*rr-40)/2; 13 px black margins top and bottom */
static void blit_bottom_slow(const u8 *src, u32 w, u32 h, u32 stride)
{
    static short ix[SCR_H];
    static u32 ix_w;
    u8 *fb0 = bot_fb[0] ? bot_fb[0] : bot_fb[1], *fb1 = bot_fb[0] ? bot_fb[1] : NULL;
    if (!fb0) return;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 t = rr * 3;
            ix[rr] = (t < 40 || (t - 40) / 2 >= w) ? -1 : (short)((t - 40) / 2);
        }
        ix_w = w;
    }
    for (u32 c = 0; c < 320; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        u32 iy = c * 3 / 2;
        const u32 *row = (const u32 *)(src + (iy < h ? iy : h - 1) * stride);
        u8 *d = fb0 + c * SCR_H * 3;
        for (u32 rr = 0; rr < SCR_H; rr++, d += 3) {
            u32 v = ix[rr] < 0 ? 0 : row[ix[rr]];
            d[0] = v;
            d[1] = v >> 8;
            d[2] = v >> 16;
        }
        if (fb1) memcpy(fb1 + c * SCR_H * 3, fb0 + c * SCR_H * 3, SCR_H * 3);
    }
}

/* Which framebuffers the LCDs show (bit 0/1: top set 0/1, bit 2/3: bottom
   set 0/1). Luma selects set 0 and nothing here ever swaps, so writing the
   other set only costs time; the ARM11 shim reads the PDC select registers
   (the ARM9 cannot reach them) and publishes them (gpu_mirror.h). Default:
   all four, as before. */
static u32 shown = 0xF;
void display_set_shown(u32 mask) { shown = mask & 0xF ? mask & 0xF : 0xF; }

/*
 * The fast copy. On hardware the old byte-per-byte copy took 300-430 ms per
 * frame (run12/13: "[monitor] CPU screen copy: ... 432.8 ms each"): the ARM9
 * reaches VRAM slowly, one transaction per access. Here every VRAM access is
 * a 16-byte LDM/STM burst:
 *  - each needed source row is read whole (320 words) into ARM9 RAM,
 *  - each output column (240 pixels BGR8 = 720 bytes) is packed into 180
 *    words in ARM9 RAM (4 pixels = 3 words) and stored with bursts,
 *  - only to the framebuffers the LCDs show, and without reading VRAM back.
 * Same pixel mapping as before (the host test compares both copies and the
 * GPU mirror byte for byte).
 */
static void wcopy(u32 *d, const u32 *s, u32 n)       /* n: multiple of 4 words */
{
#ifndef __arm__                                     /* host test build (tools/ios3ds-pica/tests) */
    while (n--) *d++ = *s++;
#else
    __asm__ volatile(
        "1: ldmia %1!, {r3, r4, r5, r12}\n"
        "   stmia %0!, {r3, r4, r5, r12}\n"
        "   subs  %2, %2, #4\n"
        "   bne   1b\n"
        : "+r"(d), "+r"(s), "+r"(n) : : "r3", "r4", "r5", "r12", "memory", "cc");
#endif
}

static u32 rowbuf[320 + 4] __attribute__((aligned(32)));    /* +1: rowbuf[w] = black */
static u32 colbuf[SCR_H * 3 / 4] __attribute__((aligned(32)));

/* 240 pixels rowbuf[ix[0..239]] -> 180 words of BGR8 */
static void pack_column(const u16 *ix)
{
    u32 *o = colbuf;
    for (u32 rr = 0; rr < SCR_H; rr += 4, o += 3) {
        u32 p0 = rowbuf[ix[rr]] & 0xFFFFFF, p1 = rowbuf[ix[rr + 1]] & 0xFFFFFF;
        u32 p2 = rowbuf[ix[rr + 2]] & 0xFFFFFF, p3 = rowbuf[ix[rr + 3]] & 0xFFFFFF;
        o[0] = p0 | p1 << 24;
        o[1] = p1 >> 8 | p2 << 16;
        o[2] = p2 >> 16 | p3 << 8;
    }
}

static void store_column(u8 *const fb[2], u32 col, u32 bits)
{
    if (!fb[1]) bits |= bits >> 1;                  /* one buffer for both sets */
    for (u32 k = 0; k < 2; k++)
        if (fb[k] && (bits >> k & 1))
            wcopy((u32 *)(fb[k] + col * SCR_H * 3), colbuf, SCR_H * 3 / 4);
}

static void blit_bottom(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 t = rr * 3;
            ix[rr] = (t < 40 || (t - 40) / 2 >= w) ? (u16)w : (u16)((t - 40) / 2);
        }
        ix_w = w;
    }
    rowbuf[w] = 0;
    for (u32 c = 0; c < 320; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        u32 iy = c * 3 / 2;
        wcopy(rowbuf, (const u32 *)(src + (iy < h ? iy : h - 1) * stride), w);
        pack_column(ix);
        store_column(bot_fb, c, shown >> 2);
    }
}

static void blit_top(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    u32 outw = h * 3 / 4, x0;
    if (outw > SCR_W) outw = SCR_W;
    x0 = (SCR_W - outw) / 2;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    for (u32 c = 0; c < outw; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        wcopy(rowbuf, (const u32 *)(src + (c * 4 / 3) * stride), w);
        pack_column(ix);
        store_column(top_fb, x0 + c, shown);
    }
}

/* ---- stereoscopic top copy ---------------------------------------------
   One source row is read per screen column (as in blit_top). The alpha byte
   of the source pixel selects which horizontal sample each eye sees:
   out(x) = in(x - stereo_eye_delta(cls, k, right)). The alpha byte is masked
   away, so the LCD always gets opaque BGR8. The bottom copy above is shared
   unchanged, including the touch map. */
static u32 eye_col[2][SCR_H] __attribute__((aligned(32)));

static void pack_eye_column(const u16 *ix, u32 w, int k)
{
    int wm1 = (int)w - 1;
    for (u32 rr = 0; rr < SCR_H; rr++) {
        int x = ix[rr], s, d;
        u32 px = rowbuf[x];
        int cls = stereo_px_class(px);
        /* class 0 (flat) and unknown alphas never move; out(x)=in(x-delta) */
        d = stereo_eye_delta(cls, k, 0);
        s = x - d;
        eye_col[0][rr] = rowbuf[s < 0 ? 0 : (s > wm1 ? wm1 : s)] & 0xFFFFFF;
        d = stereo_eye_delta(cls, k, 1);
        s = x - d;
        eye_col[1][rr] = rowbuf[s < 0 ? 0 : (s > wm1 ? wm1 : s)] & 0xFFFFFF;
    }
}

static void pack_bgr_column(const u32 *src)
{
    u32 *o = colbuf;
    for (u32 rr = 0; rr < SCR_H; rr += 4, o += 3) {
        u32 p0 = src[rr] & 0xFFFFFF, p1 = src[rr + 1] & 0xFFFFFF;
        u32 p2 = src[rr + 2] & 0xFFFFFF, p3 = src[rr + 3] & 0xFFFFFF;
        o[0] = p0 | p1 << 24;
        o[1] = p1 >> 8 | p2 << 16;
        o[2] = p2 >> 16 | p3 << 8;
    }
}

static void blit_top_stereo(const u8 *src, u32 w, u32 h, u32 stride)
{
    static u16 ix[SCR_H];
    static u32 ix_w;
    u32 outw = h * 3 / 4, x0;
    int k = stereo_shift_k(stereo_slider);
    if (outw > SCR_W) outw = SCR_W;
    x0 = (SCR_W - outw) / 2;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    for (u32 c = 0; c < outw; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        wcopy(rowbuf, (const u32 *)(src + (c * 4 / 3) * stride), w);
        pack_eye_column(ix, w, k);
        for (u32 eye = 0; eye < 2; eye++) {
            if (!top_fb[eye])
                continue;
            pack_bgr_column(eye_col[eye]);
            wcopy((u32 *)(top_fb[eye] + (x0 + c) * SCR_H * 3), colbuf, SCR_H * 3 / 4);
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

static void display_blit_slow(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    const u8 *src = (const u8 *)r[0x24 / 4];
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
    u32 stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride) return;
    if (mode != SCREEN_TOP) blit_bottom_slow(src, w, h, stride);
    if (mode == SCREEN_BOTTOM) return;
    /* screen column sx (0..399) <- source row iy; screen row from top <- source column ix */
    u32 outw = h * 3 / 4;                     /* 360 for h = 480 */
    if (outw > SCR_W) outw = SCR_W;
    u32 x0 = (SCR_W - outw) / 2;
    static u16 ix[SCR_H];
    static u32 ix_w;
    if (ix_w != w) {
        for (u32 rr = 0; rr < SCR_H; rr++) {
            u32 x = rr * 4 / 3;
            ix[rr] = x < w ? x : w - 1;
        }
        ix_w = w;
    }
    u8 *fb0 = top_fb[0] ? top_fb[0] : top_fb[1], *fb1 = top_fb[0] ? top_fb[1] : NULL;
    if (!fb0) return;
    for (u32 c = 0; c < outw; c++) {
        if (c % POLL_EVERY == 0) poll_io();
        /* memory goes bottom-to-top: source column 0 at the bottom
           (rotate the 3DS 90 deg clockwise to read it upright) */
        const u32 *row = (const u32 *)(src + (c * 4 / 3) * stride);
        u8 *d = fb0 + (x0 + c) * SCR_H * 3;
        for (u32 rr = 0; rr < SCR_H; rr++, d += 3) {
            u32 v = row[ix[rr]];
            d[0] = v;
            d[1] = v >> 8;
            d[2] = v >> 16;
        }
        if (fb1) memcpy(fb1 + (x0 + c) * SCR_H * 3, fb0 + (x0 + c) * SCR_H * 3, SCR_H * 3);
    }
}

void display_blit(u32 regs_pa)
{
    vu32 *r = (vu32 *)regs_pa;
    const u8 *src = (const u8 *)r[0x24 / 4];
    u32 w = r[0x30 / 4] >> 16 & 0x1FF, h = r[0x30 / 4] & 0x1FF;
    u32 stride = r[0x28 / 4] * 4;
    if (!w || !h || !stride) return;
    if (w <= 320 && !(w & 3) && stride >= w * 4 && !(stride & 3) && !((u32)(uintptr_t)src & 3) &&
        top_fb[0] && !((u32)(uintptr_t)top_fb[0] & 3) && (!top_fb[1] || !((u32)(uintptr_t)top_fb[1] & 3)) &&
        (!bot_fb[0] || !((u32)(uintptr_t)bot_fb[0] & 3)) && (!bot_fb[1] || !((u32)(uintptr_t)bot_fb[1] & 3))) {
        /* Experimental stereo path. With the flag off (or a slider down, or
           an unstamped frame, or SCREEN_BOTTOM) `stereo` is false and the two
           lines below are the original copy, unchanged. */
        bool stereo = stereo_cfg && !stereo_no_shim && stereo_active(1, stereo_slider) &&
                      mode != SCREEN_BOTTOM && top_fb[1] && top_fb[1] != top_fb[0] &&
                      stereo_is_home(src, w, h, stride);
        if (stereo && !stereo_hw && !gm_set_stereo(true)) {
            if (!stereo_warned) {
                log_warn("[monitor] stereo 3D: no shim to switch the top LCD into 3D mode; staying in 2D\n");
                stereo_warned = true;
            }
            stereo_no_shim = true;
            stereo = false;
        }
        if (!stereo && stereo_hw) {
            stereo_barrier(false);
            gm_set_stereo(false);
            stereo_hw = false;
            logp("[monitor] stereo 3D: top screen back to 2D, parallax barrier off\n");
        }
        if (mode != SCREEN_TOP) blit_bottom(src, w, h, stride);
        if (mode != SCREEN_BOTTOM) {
            if (stereo)
                blit_top_stereo(src, w, h, stride);
            else
                blit_top(src, w, h, stride);
        }
        if (stereo && !stereo_hw) {
            stereo_barrier(true);
            stereo_hw = true;
            logp("[monitor] stereo 3D: both eyes from the stamped home frame, parallax barrier on "
                 "(slider %u, shift %d px)\n", stereo_slider, stereo_shift_k(stereo_slider));
        }
        return;
    }
    display_blit_slow(regs_pa);
}
