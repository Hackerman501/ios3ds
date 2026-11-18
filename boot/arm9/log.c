/*
 * Logging: every line goes to the top-screen console and to sd:/ios3ds/log.txt
 * (synced after each write so a hang/power-off keeps everything written so far).
 * Lines logged before the SD card is mounted are buffered and flushed on mount.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "log.h"
#include "console.h"
#include "util.h"
#include "fatfs/ff.h"

static FATFS fs;
static FIL logf;
static bool log_open;
static char pending[16384];
static u32 pending_len;
static bool sync_each = true;
static bool dirty;
static int con_target = CON_TOP;

void log_to_bottom(bool on) { con_target = on ? CON_BOTTOM : CON_TOP; }
void log_target(int which) { con_target = which; }   /* -1: SD only */

static u32 log_errs, log_last_err;

/* a failed f_write/f_sync leaves the FIL in an error state and FatFs drops
   every later write silently: reopen in append mode and say so */
static void log_recover(FRESULT r)
{
    log_errs++;
    log_last_err = r;
    f_close(&logf);
    if (f_open(&logf, "0:/ios3ds/log.txt", FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        log_open = false;
        return;
    }
    char m[64];
    int k = snprintf(m, sizeof(m), "\n[log] write error %d (#%u), reopened\n", (int)r, (unsigned)log_errs);
    UINT bw;
    f_write(&logf, m, k, &bw);
    f_sync(&logf);
}

static void sd_write(const char *s, u32 n)
{
    if (log_open) {
        UINT bw;
        FRESULT r = f_write(&logf, s, n, &bw);
        if (r == FR_OK && sync_each) r = f_sync(&logf);
        else if (r == FR_OK) dirty = true;
        if (r != FR_OK) log_recover(r);
    } else if (pending_len + n < sizeof(pending)) {
        memcpy(pending + pending_len, s, n);
        pending_len += n;
    }
}

void log_raw(const char *s, u32 n, u32 color)
{
    if (con_target >= 0) {
        con_set_color(con_target, color);
        for (u32 i = 0; i < n; i++) con_putc(con_target, s[i]);
    }
    sd_write(s, n);
}

void log_color(u32 color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    log_raw(buf, n, color);
}

void log_sync_each(bool on)
{
    sync_each = on;
    if (on && log_open) f_sync(&logf);
    if (on) dirty = false;
}

/* sync only if something was written since the last sync: on real SD every
   f_sync rewrites the data sector and the directory entry (milliseconds) */
void log_flush(void)
{
    if (log_open && dirty) {
        FRESULT r = f_sync(&logf);
        if (r != FR_OK) log_recover(r);
    }
    dirty = false;
}

int log_mount_sd(char *why, u32 whylen)
{
    FRESULT r = f_mount(&fs, "0:", 1);
    if (r != FR_OK) {
        snprintf(why, whylen, "f_mount failed (FRESULT %d)", r);
        return -1;
    }
    f_mkdir("0:/ios3ds");
    /* keep the previous boot's log as log_prev.txt (8.3: LOG_PREV.TXT) */
    f_unlink("0:/ios3ds/log_prev.txt");
    f_rename("0:/ios3ds/log.txt", "0:/ios3ds/log_prev.txt");
    r = f_open(&logf, "0:/ios3ds/log.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        snprintf(why, whylen, "cannot create ios3ds/log.txt (FRESULT %d)", r);
        return -2;
    }
    log_open = true;
    if (pending_len) {
        UINT bw;
        f_write(&logf, pending, pending_len, &bw);
        f_sync(&logf);
        pending_len = 0;
    }
    return 0;
}

bool log_sd_ok(void) { return log_open; }

/* read a whole small file into buf; returns bytes read or -FRESULT */
int sd_read_small(const char *path, char *buf, u32 size)
{
    FIL f;
    UINT br = 0;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) return -(int)r;
    r = f_read(&f, buf, size - 1, &br);
    f_close(&f);
    if (r != FR_OK) return -(int)r;
    buf[br] = 0;
    return br;
}

/* write a whole small file (created or replaced); returns bytes written or -FRESULT */
int sd_write_small(const char *path, const char *buf, u32 len)
{
    FIL f;
    UINT bw = 0;
    FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) return -(int)r;
    r = f_write(&f, buf, len, &bw);
    f_close(&f);
    if (r != FR_OK) return -(int)r;
    return bw;
}

/* load a file to an absolute address, with a progress callback per MiB */
int sd_load(const char *path, void *dst, u32 maxsize, u32 *outsize)
{
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) return -(int)r;
    u32 sz = f_size(&f);
    if (sz > maxsize) {
        f_close(&f);
        return -1000;
    }
    u8 *p = dst;
    u32 done = 0;
    while (done < sz) {
        UINT br = 0;
        u32 chunk = sz - done > (1u << 20) ? (1u << 20) : sz - done;
        r = f_read(&f, p + done, chunk, &br);
        if (r != FR_OK || br != chunk) {
            f_close(&f);
            return r != FR_OK ? -(int)r : -1001;
        }
        done += br;
    }
    f_close(&f);
    *outsize = sz;
    return 0;
}
