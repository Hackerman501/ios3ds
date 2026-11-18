/* ios3ds ARM9: 3DS MCU (system controller). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* before XNU starts: probe the MCU, fill the mailbox (RTC first: the kernel's
   calendar reads it during boot). enable = boot.cfg mcu, rtc_offset = boot.cfg
   rtc_offset (seconds added to the RTC to get UTC). */
void mcu_init(u32 shim_pa, bool enable, int32_t rtc_offset, u32 hz);
/* monitor loop: IRQ events, sliders, battery, RTC, accelerometer, requests */
void mcu_poll(u32 now_ticks);
/* INBTN_HOME / INBTN_POWER for the input sample (boot/arm9/tsc.c) */
u16 mcu_buttons(void);
