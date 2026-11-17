/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
int blksrv_open(const char *path, u32 *sectors);
bool blksrv_poll(void);
void blksrv_stats(u32 *nserved, u32 *nerr);
u32 blksrv_sectors(void);
