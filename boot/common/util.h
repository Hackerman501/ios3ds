/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdarg.h>
#include "ios3ds.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
int strncmp(const char *a, const char *b, size_t n);
void strlcpy_(char *dst, const char *src, size_t size);
u32 parse_u32(const char *s, const char **end);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
