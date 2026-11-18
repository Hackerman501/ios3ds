/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* CODEC sound path (DAC, speaker + headphone drivers, I2S1/I2S2, MCLK) as
   libn3ds CODEC_init/soundInit leaves it. Call once before XNU starts and
   before tsc_init (it soft-resets the CODEC). false = no CODEC answered. */
bool audio_init(void);

/* Monitor loop: report stream starts/stops seen by the shim's audio emulation
   (kernel/shim/audio.c) in the SD log. shim_pa = cfg.shim_pa (0 = no shim). */
void audio_poll(u32 shim_pa);

/* boot.cfg mic=0: never touch the MIC block (iOS's input stays silent) */
void audio_set_mic(bool enabled);
