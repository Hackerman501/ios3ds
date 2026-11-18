/* ios3ds ARM9: power saving while iOS' screen is off. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

void ps_init(bool enable, u32 shim_pa, u32 ticks_per_s);
/* every monitor loop: display_on = CLCD on (display_active), ios_display =
   iOS has shown its display once. Returns true on wake (copy a frame now). */
bool ps_update(bool display_on, bool ios_display, u32 now);
/* end of a loop iteration: sleep ~2 ms in WFI if the screen is off, nothing
   is pending and md0 was quiet. Returns false = caller polls as usual. */
bool ps_sleep(bool busy, bool io_recent);
bool ps_off(void);
