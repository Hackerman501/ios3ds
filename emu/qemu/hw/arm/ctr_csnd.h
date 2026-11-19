/*
 * ios3ds QEMU board: minimal 3DS CSND model (ctr_csnd.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "system/memory.h"
#include "qom/object.h"

/* map the CSND register page at parent + offset; channels read their samples
   from the FCRAM region that starts at fcram_base (physical) */
void ctr_csnd_init(MemoryRegion *parent, hwaddr offset, MemoryRegion *fcram, hwaddr fcram_base);

/* map the MIC FIFO block (IOS3DS_MIC selects the sound source, ctr_csnd.c) */
void ctr_mic_init(MemoryRegion *parent, hwaddr offset);

/* headphone jack: GPIO2 bit 0 at parent + offset (over the Wi-Fi model's GPIO
   block) and the machine property "headphones" */
bool ctr_sound_headphones(void);
void ctr_jack_init(Object *machine, MemoryRegion *parent, hwaddr offset);
