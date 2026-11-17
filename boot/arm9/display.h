/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
#define SCREEN_TOP    0   /* iOS on the top screen, monitor log on the bottom */
#define SCREEN_BOTH   1   /* iOS on both; the bottom copy is the touch map (tap what you see) */
#define SCREEN_BOTTOM 2   /* iOS on the bottom (touch) screen only, log on the top */
#define SCREEN_MODES  3
void display_init(u8 *top0, u8 *top1, u8 *bot0, u8 *bot1);
void display_set_mode(int mode);
bool display_active(u32 regs_pa);
void display_blit(u32 regs_pa);
void display_arm(u32 regs_pa);
void display_fbs(u32 out[4]);
void display_set_shown(u32 mask);   /* bit 0/1 top set 0/1, bit 2/3 bottom set 0/1 */
