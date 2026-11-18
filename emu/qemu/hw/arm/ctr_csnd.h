/*
 * ios3ds QEMU board: minimal 3DS CSND model (ctr_csnd.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "system/memory.h"

/* map the CSND register page at parent + offset; channels read their samples
   from the FCRAM region that starts at fcram_base (physical) */
void ctr_csnd_init(MemoryRegion *parent, hwaddr offset, MemoryRegion *fcram, hwaddr fcram_base);
