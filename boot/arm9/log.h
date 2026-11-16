/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"
#include "console.h"

void log_raw(const char *s, u32 n, u32 color);
void log_color(u32 color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define logp(...) log_color(CON_WHITE, __VA_ARGS__)
#define log_ok(...) log_color(CON_GREEN, __VA_ARGS__)
#define log_warn(...) log_color(CON_YELLOW, __VA_ARGS__)
#define log_err(...) log_color(CON_RED, __VA_ARGS__)
void log_sync_each(bool on);
void log_flush(void);
int log_mount_sd(char *why, u32 whylen);
bool log_sd_ok(void);
int sd_read_small(const char *path, char *buf, u32 size);
int sd_load(const char *path, void *dst, u32 maxsize, u32 *outsize);
void log_to_bottom(bool on);
void log_target(int which);
