/* ARM9 side of the GPU display mirror (gpumirror.c). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
/* cfgv: boot.cfg gpu, gpu_mirror, gpu_vbl_div, gpu_spin_us, gpu_clk, gpu_diag, ca_gpu; mode: SCREEN_* at start */
void gm_init(u32 shim_pa, u32 tick_hz, const u32 cfgv[7], int mode);
void gm_want(bool on);
void gm_set_mode(int mode, u32 (*now)(void));
/* display_on: iOS' display is on (CLCD enabled). While it is off the GPU has
   nothing to copy; that is not "frames stopped", and after it comes back the
   GPU gets a second to resume before the CPU copies again. */
bool gm_live(u32 now, bool display_on);
u32 gm_shown(void);     /* display_set_shown() mask from the shim's PDC select readout */
/* Ask the ARM11 GPU part to switch PDC0 between its 2D and 3D (AB interlace,
   two eye framebuffers) presets. The ARM9 waits for the ack before it enables
   the parallax barrier. Returns true when acked, false if no shim is there. */
bool gm_set_stereo(bool on);
void gm_report(u32 now, u32 cpu_blits, u32 cpu_ticks, u32 regs_pa, int mode, bool shown);
