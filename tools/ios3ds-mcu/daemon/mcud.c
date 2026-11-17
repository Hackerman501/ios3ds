/*
 * ios3ds-mcud: the 3DS system controller (MCU) -> iOS userland.
 *
 *   ARM9 monitor --(mcubox_t in the shim page)--> sysctl kern.ios3ds_mcu
 *     -> this daemon:
 *        volume slider        -> AVSystemController setActiveCategoryVolumeTo:
 *        accelerometer        -> IOHID accelerometer events (type 13) into
 *                                SpringBoard's HID event system (orientation,
 *                                UIAccelerometer)
 *     <- iOS brightness: SpringBoard's HID event system properties
 *        DisplayBrightness x DisplayBrightnessFactor -> sysctl
 *        kern.ios3ds_mcuctl -> the shim writes the LCD backlight PWM
 *
 * Reading the sysctl also lets the shim refresh its IOPMPowerSource (battery
 * icon). The RTC, HOME/POWER and shell close/open never pass through here
 * (kernel calendar, input mailbox, ARM9). See docs/mcu.md.
 *
 * Arguments: debug (log every change), acc=AXES (default "-y,x,-z": iOS x,
 * y, z from the 3DS axes, see docs/mcu.md), noacc, novol, nobl.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <math.h>

#include "mcu_proto.h"

/* ---- libSystem ----------------------------------------------------------- */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t bootstrap_port;
extern mach_port_t mach_task_self_;
uint64_t mach_absolute_time(void);
kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
struct tv32 { long tv_sec; int tv_usec; };     /* armv6 Darwin timeval */
int gettimeofday(void *tv, void *tz);
struct mtb { unsigned numer, denom; };
int mach_timebase_info(struct mtb *);
uint32_t notify_register_check(const char *name, int *token);
uint32_t notify_get_state(int token, uint64_t *state);

/* ---- CoreFoundation / IOKit HID (layouts as in tools/ios3ds-input) ------- */
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFNumber *CFNumberRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
CFStringRef CFStringCreateWithCString(const void *alloc, const char *s, uint32_t enc);
unsigned long CFGetTypeID(CFTypeRef cf);
unsigned long CFNumberGetTypeID(void);
unsigned char CFNumberGetValue(CFNumberRef n, long type, void *out);
void CFRelease(CFTypeRef cf);
#define kCFStringEncodingUTF8 0x08000100u
#define kCFNumberFloatType    12

IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(const void *allocator);
CFTypeRef IOHIDEventSystemClientCopyProperty(IOHIDEventSystemClientRef client, CFStringRef key);
void IOHIDEventSystemClientDispatchEvent(IOHIDEventSystemClientRef client, IOHIDEventRef event);
/* 0x337faca4 in the 4.2.1 dyld cache: (alloc r0, timestamp r1:r2, x r3,
   y [sp], z [sp+4], options [sp+8]) -> the common 3-axis creator, type 13 */
IOHIDEventRef IOHIDEventCreateAccelerometerEvent(const void *allocator, uint64_t timeStamp,
        float x, float y, float z, uint32_t options);

/* ---- Objective-C (AVSystemController lives in Celestial.framework) ------- */
typedef void *id;
typedef void *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
#define MSG(type, obj, sel) ((type)objc_msgSend)(obj, sel_registerName(sel)

static int opt_debug, opt_noacc, opt_novol, opt_nobl;
static void say(const char *fmt, ...);

/* Clock cross-check (the open "gettimeofday vs mach_absolute_time" question,
   docs/mcu.md): the same interval measured by the 3DS RTC (ARM9 mailbox, 1 s
   resolution), gettimeofday() and mach_absolute_time(). */
static struct { struct tv32 tv; uint64_t m; uint32_t rtc; int set; } clk0;

static void clock_check(const mcubox_t *b, const char *when)
{
    struct tv32 tv;
    struct mtb tb = { 0, 0 };
    uint64_t m = mach_absolute_time();
    gettimeofday(&tv, 0);
    mach_timebase_info(&tb);
    if (!clk0.set) {
        clk0.tv = tv; clk0.m = m; clk0.rtc = b->rtc_utc; clk0.set = 1;
        return;
    }
    double gtod = (tv.tv_sec - clk0.tv.tv_sec) * 1000.0 + (tv.tv_usec - clk0.tv.tv_usec) / 1000.0;
    double mach = tb.denom ? (double)(m - clk0.m) * tb.numer / tb.denom / 1e6 : -1;
    say("[mcud] clocks %s: RTC +%d s, gettimeofday +%.0f ms, mach_absolute_time +%.0f ms (%llu ticks, timebase %u/%u)",
        when, (int)(b->rtc_utc - clk0.rtc), gtod, mach, (unsigned long long)(m - clk0.m), tb.numer, tb.denom);
}
static int accmap[3][2] = { { 1, -1 }, { 0, 1 }, { 2, -1 } };   /* iOS axis <- (3DS axis, sign) */

static void say(const char *fmt, ...)            /* one line to the console (= the 3DS log) */
{
    char line[200];
    va_list ap;
    int n, fd;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

static int parse_accmap(const char *s)
{
    int m[3][2], i = 0;
    while (i < 3 && *s) {
        int sign = 1;
        if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
        if (*s < 'x' || *s > 'z') return 0;
        m[i][0] = *s - 'x';
        m[i][1] = sign;
        s++;
        i++;
        if (*s == ',') s++;
    }
    if (i != 3) return 0;
    memcpy(accmap, m, sizeof m);
    return 1;
}

/* ---- volume -------------------------------------------------------------- */
static id avsc;

static int volume_set(float v)
{
    if (!avsc) {
        id cls;
        dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_LAZY);
        cls = objc_getClass("AVSystemController");
        if (cls) avsc = MSG(id (*)(id, SEL), cls, "sharedAVSystemController"));
        if (!avsc) return 0;
    }
    return MSG(signed char (*)(id, SEL, float), avsc, "setActiveCategoryVolumeTo:"), v) != 0;
}

/* ---- brightness ---------------------------------------------------------- */
static IOHIDEventSystemClientRef client;
static CFStringRef k_bright, k_factor;

static int copy_float(CFStringRef key, float *out)
{
    CFTypeRef v = client ? IOHIDEventSystemClientCopyProperty(client, key) : 0;
    int ok = 0;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            ok = CFNumberGetValue((CFNumberRef)v, kCFNumberFloatType, out);
        CFRelease(v);
    }
    return ok;
}

int main(int argc, char **argv)
{
    int mib[8], cmib[8], i;
    size_t miblen = 8, cmiblen = 8, len;
    mcubox_t box;
    struct { uint32_t acc, backlight, _r[2]; } ctl, ctl_sent;
    uint32_t loops = 0, seen_seq = 0, acc_seen = 0, waits = 0;
    int synced = 0, vol_last = -1, vol_pending = 0;
    float g_avg = 0, bright = -1, factor = -1;
    int acc_dom = -1, orient_tok = -1;
    uint32_t orient_check = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "debug")) opt_debug = 1;
        else if (!strcmp(argv[i], "noacc")) opt_noacc = 1;
        else if (!strcmp(argv[i], "novol")) opt_novol = 1;
        else if (!strcmp(argv[i], "nobl")) opt_nobl = 1;
        else if (!strncmp(argv[i], "acc=", 4) && !parse_accmap(argv[i] + 4))
            say("[mcud] bad %s (want e.g. acc=-y,x,-z); keeping the default", argv[i]);
    }

    while (sysctlnametomib(MCU_SYSCTL_NAME, mib, &miblen) != 0 ||
           sysctlnametomib("kern.ios3ds_mcuctl", cmib, &cmiblen) != 0) {
        if (!waits++) say("[mcud] no %s (shim without MCU support); waiting", MCU_SYSCTL_NAME);
        miblen = cmiblen = 8;
        sleep(10);
    }
    memset(&ctl, 0, sizeof ctl);
    memset(&ctl_sent, 0xFF, sizeof ctl_sent);
    k_bright = CFStringCreateWithCString(0, "DisplayBrightness", kCFStringEncodingUTF8);
    k_factor = CFStringCreateWithCString(0, "DisplayBrightnessFactor", kCFStringEncodingUTF8);

    for (;; loops++) {
        len = sizeof box;
        if (sysctl(mib, (unsigned)miblen, &box, &len, 0, 0) != 0 || len != sizeof box ||
            box.magic != MCU_MAGIC || box.version != MCU_VERSION || box.state != MCU_ST_OK) {
            if (!synced && !waits++)
                say("[mcud] MCU mailbox not live (magic %08x state %u): nothing to do",
                    (unsigned)box.magic, (unsigned)box.state);
            sleep(5);
            continue;
        }
        if (!synced) {
            synced = 1;
            say("[mcud] MCU version %02x/%02x: battery %u%%%s%s, volume slider %u/63, shell %s, RTC %u",
                box.fw >> 8, box.fw & 0xFF, box.batt_percent, (box.flags & MCUF_ADAPTER) ? ", charger" : "",
                (box.flags & MCUF_CHARGING) ? ", charging" : "", box.vol_raw,
                (box.flags & MCUF_SHELL_OPEN) ? "open" : "closed", (unsigned)box.rtc_utc);
            clock_check(&box, "start");
        }
        if (loops == 600) clock_check(&box, "after ~60 s");
        if (loops == 3000) clock_check(&box, "after ~300 s");

        /* SpringBoard owns the HID event system; connect once it is there */
        if (!client && (loops % 20) == 0) {
            mach_port_t svc = 0;
            if (bootstrap_look_up(bootstrap_port, "com.apple.iohideventsystem", &svc) == 0) {
                mach_port_deallocate(mach_task_self_, svc);
                client = IOHIDEventSystemClientCreate(0);
                if (client) say("[mcud] connected to SpringBoard's HID event system");
            }
        }

        /* volume slider: absolute, 0..63 -> 0..1; move iOS only when the
           slider moved (by 2 raw steps or to an end), so iOS' own buttons
           (L/R) keep working in between */
        if (!opt_novol && (box.valid & MCUV_SLIDERS)) {
            int v = box.vol_raw;
            if (vol_last < 0 || abs(v - vol_last) >= 2 || ((v == 0 || v == 63) && v != vol_last)) {
                vol_last = v;
                vol_pending = 1;
            }
            if (vol_pending && client) {
                float f = v / 63.0f;
                if (volume_set(f)) {
                    vol_pending = 0;
                    if (opt_debug || loops < 50) say("[mcud] volume slider %d/63 -> iOS volume %.2f", v, f);
                } else if ((loops % 100) == 0) {
                    say("[mcud] AVSystemController not available yet (volume %d/63 pending)", v);
                }
            }
        }

        /* iOS brightness -> LCD backlight, 2x per second */
        if (!opt_nobl && client && (loops % 5) == 0) {
            float b = -1, fa = -1;
            int hb = copy_float(k_bright, &b), hf = copy_float(k_factor, &fa);
            if (hb && !hf) fa = 1;
            if (hb && (fabsf(b - bright) > 0.005f || fabsf(fa - factor) > 0.005f)) {
                say("[mcud] iOS brightness %.2f x factor %.2f -> LCD backlight %u/1000", b, fa,
                    (unsigned)(b * fa * 1000.0f + 0.5f));
                bright = b;
                factor = fa;
            }
            if (hb) {
                float l = bright * factor;
                if (l < 0) l = 0;
                if (l > 1) l = 1;
                ctl.backlight = 1 + (uint32_t)(l * 1000.0f + 0.5f);
            } else if (bright < -0.5f && loops == 50) {
                say("[mcud] SpringBoard reports no DisplayBrightness: LCD backlight left as Luma set it");
            }
        }

        /* accelerometer while the screen is on */
        ctl.acc = !opt_noacc && client && factor != 0.0f;
        if (ctl.acc && box.acc_samples != acc_seen && (box.valid & MCUV_ACC)) {
            float r[3], g, out[3];
            acc_seen = box.acc_samples;
            for (i = 0; i < 3; i++) r[i] = box.acc[i];
            g = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (g > 1.0f) {
                /* scale: the running mean of |a| is 1 g (the raw unit is not
                   documented; this makes the mapping independent of it) */
                g_avg = g_avg > 0 ? g_avg * 0.98f + g * 0.02f : g;
                for (i = 0; i < 3; i++) out[i] = accmap[i][1] * r[accmap[i][0]] / g_avg;
                IOHIDEventRef ev = IOHIDEventCreateAccelerometerEvent(0, mach_absolute_time(),
                                                                      out[0], out[1], out[2], 0);
                if (ev) {
                    IOHIDEventSystemClientDispatchEvent(client, ev);
                    CFRelease(ev);
                }
                /* log when the dominant axis changes, with SpringBoard's own
                   view of it (notify state com.apple.springboard.rawOrientation) */
                {
                    int dom = 0, k;
                    for (k = 1; k < 3; k++) if (fabsf(out[k]) > fabsf(out[dom])) dom = k;
                    dom = dom * 2 + (out[dom] < 0);
                    if (dom != acc_dom) {
                        uint64_t st = 0;
                        if (orient_tok < 0) notify_register_check("com.apple.springboard.rawOrientation", &orient_tok);
                        if (orient_tok >= 0) notify_get_state(orient_tok, &st);
                        say("[mcud] accelerometer raw %d %d %d -> iOS %.2f %.2f %.2f g (SpringBoard rawOrientation %u)",
                            box.acc[0], box.acc[1], box.acc[2], out[0], out[1], out[2], (unsigned)st);
                        acc_dom = dom;
                        orient_check = loops + 30;
                    }
                }
            }
        }

        if (orient_check && loops == orient_check && orient_tok >= 0) {
            uint64_t st = 0;
            notify_get_state(orient_tok, &st);
            say("[mcud] SpringBoard rawOrientation 3 s later: %u", (unsigned)st);
            orient_check = 0;
        }
        if (memcmp(&ctl, &ctl_sent, sizeof ctl)) {
            if (sysctl(cmib, (unsigned)cmiblen, 0, 0, &ctl, sizeof ctl) == 0) {
                if (ctl.acc != ctl_sent.acc) say("[mcud] accelerometer %s", ctl.acc ? "on" : "off");
                ctl_sent = ctl;
            }
        }
        if (opt_debug && box.seq != seen_seq && (loops % 50) == 0)
            say("[mcud] seq %u: battery %u%% flags %02x volume %u 3D %u irq %08x home %u power %u/%u shell %u/%u",
                (unsigned)box.seq, box.batt_percent, box.flags, box.vol_raw, box.slider3d, (unsigned)box.irq_seen,
                (unsigned)box.home_presses, (unsigned)box.power_presses, (unsigned)box.power_holds,
                (unsigned)box.shell_closes, (unsigned)box.shell_opens);
        seen_seq = box.seq;
        usleep(100000);
    }
}
