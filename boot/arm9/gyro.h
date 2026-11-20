/* ios3ds ARM9: the 3DS gyroscope (I2C bus 3). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* after mcu_init (the gyro shares the MCU mailbox): probe the chip, leave it
   asleep. enable = boot.cfg gyro, axes = boot.cfg gyro_axes ("-y,x,z", NULL =
   default). */
void gyro_init(u32 shim_pa, bool enable, const char *axes, u32 hz);
/* monitor loop: wakes / sleeps the chip as iOS asks, samples it at 100 Hz */
void gyro_poll(u32 now_ticks);
