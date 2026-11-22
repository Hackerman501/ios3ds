/*
 * Auto-brightness curve, shared by ios3ds-mcud and the host tests.
 *
 * The only documented ambient-light source on a New 3DS is the face-tracking
 * ("inner") camera: the QTM sysmodule publishes its luminance in lux through
 * qtm:s GetCameraLux (3dbrew "QTM Services" 0x04020000, libctru
 * QTMS_GetCameraLuminance). From the ARM9 the same sensor's auto-exposure
 * reports its measured luminance as an 8-bit value
 * (Aptina MCU variable MCUREG_APT_AE_LAST_LUMA = 0x2218, BlocksDS
 * libnds/nds/camera.h). The ARM9 puts that raw byte in the MCU mailbox; the
 * conversion to an approximate lux scale and the brightness curve live here.
 *
 * The mapping is deliberately coarse: a camera's AE luma is gamma-encoded,
 * affected by auto-exposure and by what the camera sees, so these are *not*
 * calibrated lux.Smoothing and hysteresis are applied afterwards so a moving
 * scene never makes the backlight flicker. See docs/mcu.md section 12.
 *
 * Pure C, no floats on the ARM9 side: only the daemon and host tests include
 * this header. SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <math.h>

/* Anchor points of the approximate log-lux scale: luma 0,32,64,...,224,255
   <-> 0,1,3,10,30,100,300,1000,3000 lux. QEMU's camera model implements the
   inverse mapping for the `ambient-light` QOM property. */
#define ALS_LUMA_ANCHORS 9u
#define ALS_LUX_MAX      3000.0f

static inline float als_luma_to_lux(uint8_t luma)
{
    static const float lux[ALS_LUMA_ANCHORS] = {
        0.0f, 1.0f, 3.0f, 10.0f, 30.0f, 100.0f, 300.0f, 1000.0f, 3000.0f
    };
    if (luma >= 255) return ALS_LUX_MAX;
    unsigned i = (unsigned)luma / 32u;
    if (i >= ALS_LUMA_ANCHORS - 1) return ALS_LUX_MAX;
    unsigned off = (unsigned)luma - i * 32u;
    float a = lux[i], b = lux[i + 1];
    /* geometric interpolation: equal luma steps are equal stops of light */
    if (a <= 0.0f) return b * (float)off / 32.0f;
    return a * powf(b / a, (float)off / 32.0f);
}

/* Target backlight multiplier around the user's own brightness setting.
   Dark room: 0.45x (the display would glare), bright room: 1.0x. The anchors
   follow the same log-lux scale; below/above them the factor is clamped. */
#define ALS_FACTOR_MIN 0.45f
#define ALS_FACTOR_MAX 1.00f

static inline float als_lux_to_factor(float lux)
{
    static const float anchor_lux[4] = { 0.1f, 10.0f, 100.0f, 1000.0f };
    static const float anchor_factor[4] = { 0.45f, 0.60f, 0.80f, 1.00f };
    if (lux <= anchor_lux[0]) return ALS_FACTOR_MIN;
    if (lux >= 1000.0f) return ALS_FACTOR_MAX;
    int i = 0;
    while (i < 2 && lux >= anchor_lux[i + 1]) i++;
    /* logarithmic interpolation inside the decade */
    float f = logf(lux / anchor_lux[i]) / logf(anchor_lux[i + 1] / anchor_lux[i]);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return anchor_factor[i] + f * (anchor_factor[i + 1] - anchor_factor[i]);
}

/* Stateful smoothing. The daemon calls this at 2 Hz (4 Hz with the screen
   off); every call passes the newest raw luminance and elapsed milliseconds.

   Hysteresis: a new target must differ by at least 0.03 of the multiplier and
   survive two consecutive samples before it is accepted. Smoothing: the
   accepted target is approached at no more than 0.15 per second (0.015 for a
   100 ms call). This makes single-frame changes (a hand over the camera, a
   passing shadow) invisible and keeps ordinary transitions flicker-free. */
#define ALS_DEADBAND   0.03f
#define ALS_SLEW_MS    0.00015f    /* multiplier per millisecond */

typedef struct {
    uint8_t  valid;        /* 0: the next call snaps to the current light level */
    uint8_t  votes;        /* consecutive samples asking for a new target */
    float    target;       /* accepted (hysteretic) target */
    float    current;      /* value actually applied */
    int8_t   direction;    /* sign of the pending change */
    uint16_t _pad;
} als_smooth_t;

static inline float als_smooth(als_smooth_t *st, uint8_t luma, uint32_t dt_ms)
{
    float want = als_lux_to_factor(als_luma_to_lux(luma));
    if (!st->valid) {
        st->valid = 1;
        st->votes = 0;
        st->target = st->current = want;
        st->direction = 0;
        return st->current;
    }
    float delta = want - st->target;
    if (fabsf(delta) >= ALS_DEADBAND) {
        int8_t dir = delta > 0 ? 1 : -1;
        if (dir != st->direction) {
            st->direction = dir;
            st->votes = 0;
        }
        if (++st->votes >= 2) {
            st->target = want;
            st->votes = 0;
        }
    } else {
        st->votes = 0;
        st->direction = 0;
    }
    float gap = st->target - st->current;
    if (fabsf(gap) > 0.0005f) {
        float step = (float)dt_ms * ALS_SLEW_MS;
        if (step < 0.002f) step = 0.002f;       /* at least one small step per call */
        if (step > 0.05f) step = 0.05f;         /* never jump after a long sleep */
        if (fabsf(gap) <= step) st->current = st->target;
        else st->current += (gap > 0 ? step : -step);
    }
    if (st->current < ALS_FACTOR_MIN) st->current = ALS_FACTOR_MIN;
    if (st->current > ALS_FACTOR_MAX) st->current = ALS_FACTOR_MAX;
    return st->current;
}
