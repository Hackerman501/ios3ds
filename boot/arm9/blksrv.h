/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
int blksrv_open(const char *path, u32 *sectors);
/* Set the HFS+ clean-unmount flag after a clean iOS shutdown (see blksrv.c). */
bool blksrv_mark_clean(void);
bool blksrv_poll(void);
/* Idle/dirty write-back from the monitor loop (ticks: ARM9 timer 2+3). */
void blksrv_tick(u32 ticks);
void blksrv_stats(u32 *nserved, u32 *nerr);
u32 blksrv_sectors(void);
void blksrv_profile(u32 level, u32 (*now)(void), u32 hz);
void blksrv_prof_tick(void);
