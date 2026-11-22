/* ios3ds ARM9: 3DS MCU (system controller). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* before XNU starts: probe the MCU, fill the mailbox (RTC first: the kernel's
   calendar reads it during boot). enable = boot.cfg mcu, rtc_offset = boot.cfg
   rtc_offset (seconds added to the RTC to get UTC). */
void mcu_init(u32 shim_pa, bool enable, int32_t rtc_offset, u32 hz);
/* boot.cfg mcu_acc=0: the shim hides the accelerometer from iOS (after mcu_init) */
void mcu_config_acc(bool on);
/* power saving while iOS' screen is off (docs/mcu.md section 9) */
void mcu_set_powersave(bool on);
void mcu_screen_off(bool off);
/* boot.cfg notify_led=1: low battery (<= 10 %, not charging) blinks the notification LED red */
void mcu_set_led(bool on);
/* boot.cfg auto_brightness on New models: sample the inner camera's ambient
   luminance into the MCU mailbox (docs/mcu.md section 12). Call after the
   model check, once camera_init has run. */
void mcu_set_als(bool on);
/* monitor loop: IRQ events, sliders, battery, RTC, accelerometer, requests */
void mcu_poll(u32 now_ticks);
/* INBTN_HOME / INBTN_POWER for the input sample (boot/arm9/tsc.c) */
u16 mcu_buttons(void);
