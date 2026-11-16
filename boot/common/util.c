/*
 * Freestanding libc subset + vsnprintf shared by the ARM9 and ARM11 code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include "util.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if ((((uintptr_t)d | (uintptr_t)s | n) & 3) == 0) {
        u32 *dw = (u32 *)d;
        const u32 *sw = (const u32 *)s;
        for (size_t i = 0; i < n / 4; i++) {
            dw[i] = sw[i];
        }
        return dst;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    if ((((uintptr_t)d | n) & 3) == 0) {
        u32 v = (u8)c * 0x01010101u;
        for (size_t i = 0; i < n / 4; i++) {
            ((u32 *)d)[i] = v;
        }
        return dst;
    }
    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] - y[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return c ? NULL : (char *)s;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) {
            return (u8)a[i] - (u8)b[i];
        }
    }
    return 0;
}

void strlcpy_(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    if (!size) {
        return;
    }
    for (; i + 1 < size && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

u32 parse_u32(const char *s, const char **end)
{
    u32 v = 0;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++) {
            char c = *s;
            if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
            else break;
        }
    } else {
        for (; *s >= '0' && *s <= '9'; s++) {
            v = v * 10 + (*s - '0');
        }
    }
    if (end) {
        *end = s;
    }
    return v;
}

/* ---- vsnprintf: %s %c %d %u %x %X %p %%, width, '0' pad, '-' left ---- */
typedef struct {
    char *buf;
    size_t size, pos;
} outbuf_t;

static void out_c(outbuf_t *o, char c)
{
    if (o->pos + 1 < o->size) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

static void out_num(outbuf_t *o, u32 v, int base, bool upper, bool neg, int width, char pad, bool left)
{
    char tmp[12];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg) {
        tmp[n++] = '-';
    }
    if (!left) {
        for (int i = n; i < width; i++) {
            out_c(o, pad);
        }
    }
    while (n) {
        out_c(o, tmp[--n]);
    }
    if (left) {
        for (int i = n; i < width; i++) {
            out_c(o, ' ');
        }
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    outbuf_t o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_c(&o, *fmt);
            continue;
        }
        fmt++;
        bool left = false;
        char pad = ' ';
        int width = 0;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') {
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            if (!s) s = "(null)";
            len = strlen(s);
            if (!left) for (int i = len; i < width; i++) out_c(&o, ' ');
            while (*s) out_c(&o, *s++);
            if (left) for (int i = len; i < width; i++) out_c(&o, ' ');
            break;
        }
        case 'c': out_c(&o, (char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            out_num(&o, v < 0 ? -(u32)v : (u32)v, 10, false, v < 0, width, pad, left);
            break;
        }
        case 'u': out_num(&o, va_arg(ap, u32), 10, false, false, width, pad, left); break;
        case 'x': out_num(&o, va_arg(ap, u32), 16, false, false, width, pad, left); break;
        case 'X': out_num(&o, va_arg(ap, u32), 16, true, false, width, pad, left); break;
        case 'p': out_c(&o, '0'); out_c(&o, 'x');
                  out_num(&o, (u32)va_arg(ap, void *), 16, false, false, 8, '0', false); break;
        case '%': out_c(&o, '%'); break;
        case 0: fmt--; break;
        default: out_c(&o, '%'); out_c(&o, *fmt); break;
        }
    }
    if (size) {
        buf[o.pos < size ? o.pos : size - 1] = 0;
    }
    return o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
