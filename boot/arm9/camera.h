/* ios3ds ARM9: 3DS cameras, control side (docs/camera.md). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

void camera_init(u32 shim_pa, bool enable);   /* before XNU: mailbox, boot.cfg camera= */
void camera_poll(void);                       /* monitor loop: serve the shim's requests */
