/*
 * iOS brightness -> 3DS LCD backlight (docs/mcu.md section 4/5).
 *
 * Two limits matter, and they are kept in one place so the daemon and the
 * kernel shim cannot disagree:
 *
 *  - iOS level 0 with the screen ON is not "off" on a real iPod: its minimum
 *    is still readable. ios3ds-mcud clamps it to IOS_BL_MIN_LEVEL.
 *  - level 0 in the mailbox really does mean OFF (iOS lock, factor 0). The
 *    ARM9 uses `backlight == 1` to decide whether closing the shell has to
 *    lock iOS, so the shim must keep mapping level 0 to PWM 0.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"

#define IOS_BL_MIN_LEVEL 120u       /* 0.12: brightness slider at 0 (real iPods stay lit) */
#define BL_PWM_MIN       0x26u       /* libn3ds/Luma's darkest panel setting that is still readable */
#define BL_PWM_MAX       0x8Fu       /* Luma's brightest preset is 0x5F; keep headroom */

/* SpringBoard's DisplayBrightness x DisplayBrightnessFactor -> 0..1000.
   factor 0 means the display is off; do not clamp that away. */
static inline u32 ios_backlight_level(float brightness, float factor)
{
    float l;
    if (factor <= 0.0f) return 0;
    l = brightness * factor;
    if (!(l > 0.0f)) l = 0;                      /* also catches NaN / negative */
    if (l < (float)IOS_BL_MIN_LEVEL / 1000.0f) l = (float)IOS_BL_MIN_LEVEL / 1000.0f;
    if (l > 1.0f) l = 1.0f;
    return (u32)(l * 1000.0f + 0.5f);
}

/* level 0 = off (both LCDs); any lit level stays in the panel's safe range. */
static inline u32 ios_backlight_pwm(u32 level)
{
    if (!level) return 0;
    if (level > 1000) level = 1000;
    return BL_PWM_MIN + level * (BL_PWM_MAX - BL_PWM_MIN) / 1000;
}
