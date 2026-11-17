/* ARM9 side of the GPU display mirror (gpumirror.c). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
/* cfgv: boot.cfg gpu, gpu_mirror, gpu_vbl_div, gpu_spin_us, gpu_clk, gpu_diag; mode: SCREEN_* at start */
void gm_init(u32 shim_pa, u32 tick_hz, const u32 cfgv[6], int mode);
void gm_want(bool on);
void gm_set_mode(int mode, u32 (*now)(void));
bool gm_live(u32 now);
void gm_report(u32 now, u32 cpu_blits, u32 cpu_ticks, u32 regs_pa, int mode, bool shown);
