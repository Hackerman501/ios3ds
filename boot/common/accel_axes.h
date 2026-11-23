/*
 * 3DS MCU accelerometer -> iOS device axes / UIDeviceOrientation
 * (docs/mcu.md section 5, issue #1). Shared by the kernel shim and the host
 * test in tests/test_orientation.py.
 *
 * The console is held with the left side up when iOS is upright (the 3DS is
 * turned 90 degrees clockwise). Forces are +1 g against gravity; the values
 * are raw MCU units (the sign, not the scale, selects the orientation).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"

/* Effective iOS axes after the driver's device-tree 'orientation 4' flag:
   iOS x = -3DS y, iOS y = -3DS x, iOS z = +3DS z. */
static inline void ios_accel_axes(const int16_t mcu[3], int out[3])
{
    out[0] = -mcu[1];
    out[1] = -mcu[0];
    out[2] = mcu[2];
}

static inline int acc_abs_(int v) { return v < 0 ? -v : v; }

/* Which axis dominates a still sample. -1 if there is no clear gravity axis. */
static inline int ios_accel_dominant(const int v[3])
{
    int d = 0;
    for (int i = 1; i < 3; i++)
        if (acc_abs_(v[i]) > acc_abs_(v[d])) d = i;
    return acc_abs_(v[d]) * 4 > acc_abs_(v[0]) + acc_abs_(v[1]) + acc_abs_(v[2]) ? d : -1;
}

/* UIDeviceOrientation from effective iOS axes: 1 portrait, 2 portrait upside
   down, 3 landscape (home button right), 4 landscape (home button left),
   5 face up, 6 face down. 0 = unknown. The 180 degree case is 2: iOS y > 0. */
static inline int ios_device_orientation(const int v[3])
{
    int d = ios_accel_dominant(v);
    if (d < 0) return 0;
    return d == 0 ? (v[0] > 0 ? 4 : 3)
         : d == 1 ? (v[1] > 0 ? 2 : 1)
                  : (v[2] > 0 ? 5 : 6);
}
