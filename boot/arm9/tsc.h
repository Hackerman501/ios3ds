/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
#include "input_proto.h"

/* Bring up SPI bus 2 + the CODEC's touch/circle-pad ADC. Returns false if the
   CODEC does not answer (e.g. the QEMU board, where the SPI block is a stub). */
bool tsc_init(void);

/* raw CODEC access (page, reg), shared with boot/arm9/audio.c */
void cdc_bus_init(void);
bool cdc_bus_dead(void);
void cdc_forget_page(void);
void cdc_write(u8 page, u8 reg, u8 val);
void cdc_read(u8 page, u8 reg, u32 *out, u32 len);
u8 cdc_read8(u8 page, u8 reg);
void cdc_mask(u8 page, u8 reg, u8 val, u8 mask);

/* One raw reading. Returns false when the CODEC has no new conversion. */
bool tsc_read(bool *pen, u16 *tx, u16 *ty, int16_t *cx, int16_t *cy);

/* Monitor-loop hook: sample pad + touch and push changes into the shim's
   input mailbox. Call every ~8-16 ms; shim_pa = cfg.shim_pa (0 = no shim). */
void input_poll(u32 shim_pa);
/* iOS' screen is off: skip the touch ADC (buttons are still read) */
void input_set_idle(bool idle);
