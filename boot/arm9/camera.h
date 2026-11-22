/* ios3ds ARM9: 3DS cameras, control side (docs/camera.md). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

void camera_init(u32 shim_pa, bool enable, bool self_test);   /* before XNU: mailbox, boot.cfg camera=, diag= */
void camera_poll(void);                       /* monitor loop: serve the shim's requests */
/* Auto-brightness (New models only): keep the inner camera's auto-exposure
   running and read its measured luminance (Aptina MCU var 0x2218). Returns
   false when no sensor answers (then auto-brightness stays inactive). */
void camera_als_enable(bool on);
bool camera_als_sample(uint8_t *luma);
