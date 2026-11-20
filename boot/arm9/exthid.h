/* ios3ds ARM9: New 3DS C-stick + ZL/ZR (I2C bus 3, 0x54). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* enable = New 3DS and boot.cfg n3ds_hid; diag = log raw reports */
void exthid_init(bool enable, bool diag, u32 hz);
/* monitor loop: reads the chip at ~60 Hz */
void exthid_poll(u32 now_ticks);
/* the latest state in inmbox_t.ext format (input_proto.h INEXT_*); 0 = none */
u32 exthid_state(void);
