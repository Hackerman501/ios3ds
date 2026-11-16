/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
void display_init(u8 *top0, u8 *top1);
bool display_active(u32 regs_pa);
void display_blit(u32 regs_pa);
void display_arm(u32 regs_pa);
