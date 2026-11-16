/*
 * ios3ds-input: turn 3DS buttons + touch panel into iOS 4.2.1 HID events.
 *
 *   ARM9 monitor --(inmbox_t in the shim page)--> sysctl kern.ios3ds_input
 *     --> this daemon --> IOHIDEventSystemClientDispatchEvent()
 *     --> SpringBoard's own IOHIDEventSystem callback (SBHIDInterface)
 *
 * SpringBoard 4.2.1 hosts the Mach service com.apple.iohideventsystem and
 * handles events dispatched by clients exactly like events from a kernel HID
 * service (IOKit __io_hideventsystem_dispatch_event -> _IOHIDEventSystemDispatchEvent
 * -> the callback SpringBoard passed to IOHIDEventSystemOpen, service = NULL).
 * That callback turns digitizer events into GSEvent hand events, finds the
 * window/app under the finger and routes them, and turns consumer-page
 * keyboard events into menu/lock/volume button GSEvents - so we never build a
 * GSEventRecord ourselves and need no app/port bookkeeping.
 * Details and the disassembly notes: docs/input-design.md.
 *
 * Pure C, declarations local (stub SDK). Build: make (uses scripts/ios-cc).
 * Arguments: flipx flipy stretch debug   (see map_touch()).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "../input_proto.h"

/* ---- libSystem ----------------------------------------------------- */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
extern mach_port_t bootstrap_port;
uint64_t mach_absolute_time(void);
kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
kern_return_t mach_port_type(mach_port_t task, mach_port_t name, unsigned *type);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
#define MACH_PORT_TYPE_DEAD_NAME 0x100000u

/* ---- IOKit HID event SPI (IOKit.framework, 4.2.1 armv6) -------------
 * Argument layout verified against the dyld cache:
 *   IOHIDEventCreateDigitizerEvent        0x337fa3a8
 *   IOHIDEventCreateDigitizerFingerEvent  0x337fa568
 *   IOHIDEventCreateKeyboardEvent         0x337fa9dc
 *   IOHIDEventAppendEvent                 0x337fa114
 *   IOHIDEventSystemClientCreate          0x337fd5fc
 *   IOHIDEventSystemClientDispatchEvent   0x337fcad8
 * AbsoluteTime is a uint64 in r1:r2 (APCS, no register-pair alignment);
 * IOHIDFloat is a 32-bit float passed in core registers / on the stack
 * (stored 16.16 fixed); the Booleans are read as full words by the callee,
 * so they are declared int here.
 */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;

IOHIDEventRef IOHIDEventCreateDigitizerEvent(const void *allocator, uint64_t timeStamp,
        uint32_t transducerType, uint32_t index, uint32_t identity, uint32_t eventMask,
        uint32_t buttonMask, float x, float y, float z, float tipPressure,
        float barrelPressure, int range, int touch, uint32_t options);
IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(const void *allocator, uint64_t timeStamp,
        uint32_t index, uint32_t identity, uint32_t eventMask, float x, float y, float z,
        float tipPressure, float twist, int range, int touch, uint32_t options);
IOHIDEventRef IOHIDEventCreateKeyboardEvent(const void *allocator, uint64_t timeStamp,
        uint32_t usagePage, uint32_t usage, int down, uint32_t options);
void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child);
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(const void *allocator);
void IOHIDEventSystemClientDispatchEvent(IOHIDEventSystemClientRef client, IOHIDEventRef event);
void CFRelease(const void *cf);

#define kIOHIDDigitizerTransducerTypeHand 3
#define kIOHIDDigitizerEventRange    0x01
#define kIOHIDDigitizerEventTouch    0x02
#define kIOHIDDigitizerEventPosition 0x04

/* HID usages SpringBoard's keyboard handler (sub_5028c) acts on */
#define kHIDPage_Consumer            0x0C
#define kHIDUsage_Csmr_Power         0x30   /* lock button  -> GSEvent 1010/1011 */
#define kHIDUsage_Csmr_Menu          0x40   /* home button  -> GSEvent 1000/1001 */
#define kHIDUsage_Csmr_VolumeInc     0xE9   /*              -> GSEvent 1006/1007 */
#define kHIDUsage_Csmr_VolumeDec     0xEA   /*              -> GSEvent 1008/1009 */

/* IOHIDEventSystemClient: +8 = send right to the per-client server port
   that io_hideventsystem_open() returned (0 if SpringBoard was not there). */
#define CLIENT_PORT(c) (*(mach_port_t *)((char *)(c) + 8))

/* ---- 3DS pad bits (boot/common/ios3ds.h HID_*) --------------------- */
#define PAD_SELECT (1u << 2)
#define PAD_START  (1u << 3)
#define PAD_R      (1u << 8)
#define PAD_L      (1u << 9)

static const struct { uint16_t btn, usage; } keymap[] = {
    { PAD_START,  kHIDUsage_Csmr_Menu },
    { INBTN_HOME, kHIDUsage_Csmr_Menu },
    { PAD_SELECT, kHIDUsage_Csmr_Power },
    { INBTN_POWER, kHIDUsage_Csmr_Power },
    { PAD_R,      kHIDUsage_Csmr_VolumeInc },
    { PAD_L,      kHIDUsage_Csmr_VolumeDec },
};

static int opt_flipx, opt_flipy, opt_stretch, opt_debug;
static IOHIDEventSystemClientRef client;

static void say(const char *fmt, ...)
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

/*
 * Bottom-screen panel -> normalised iOS screen position (0..1, origin top-left
 * of the 320x480 portrait UI).
 *
 * The ARM9 shows iOS on the top screen with iOS y along the 3DS x axis and
 * iOS x along the 3DS y axis (boot/arm9/display.c: column = 20 + iy*3/4,
 * row-from-top = ix*3/4). The touch panel uses the same axes, so:
 *     ny = panel x / 320            (480 pt over 320 px: 2/3 px per pt)
 *     nx = (panel y - 13.3) / 213.3 (same 2/3 scale, centred; the 13 px
 *                                    margins clamp to the screen edge)
 * i.e. the bottom screen is an absolute, aspect-true map of the iOS screen -
 * and becomes direct touch if the ARM9 also mirrors iOS there at 2/3 scale.
 * `stretch` uses the whole 240 px instead; `flipx`/`flipy` mirror an axis
 * (flipx is needed if display_blit is changed to ix = rr*4/3).
 */
static void map_touch(const insample_t *s, float *nx, float *ny)
{
    float px = (float)s->tx * (320.0f / 4096.0f);
    float py = (float)s->ty * (240.0f / 4096.0f);
    float x, y = px / 320.0f;

    if (opt_stretch) x = py / 240.0f;
    else             x = (py - (240.0f - 640.0f / 3.0f) / 2.0f) / (640.0f / 3.0f);
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;
    *nx = opt_flipx ? 1.0f - x : x;
    *ny = opt_flipy ? 1.0f - y : y;
}

/* One hand event with one finger child - the shape SBHIDInterface expects:
 *   parent: eventMask (Range|Touch on down/up, Position on move), touch, x, y
 *   child:  index, identity (<= 5), x, y, pressure, range, touch
 * Down = Range|Touch + touch 1, move = Position + touch 1,
 * up = Range|Touch + touch 0 (the child must still be present). */
static void send_touch(float nx, float ny, int touch, uint32_t mask)
{
    uint64_t now = mach_absolute_time();
    IOHIDEventRef hand, finger;

    hand = IOHIDEventCreateDigitizerEvent(0, now, kIOHIDDigitizerTransducerTypeHand,
            0, 0, mask, 0, nx, ny, 0.0f, 0.0f, 0.0f, touch, touch, 0);
    if (!hand) return;
    finger = IOHIDEventCreateDigitizerFingerEvent(0, now, 1, 2, mask, nx, ny, 0.0f,
            touch ? 1.0f : 0.0f, 0.0f, touch, touch, 0);
    if (finger) {
        IOHIDEventAppendEvent(hand, finger);
        IOHIDEventSystemClientDispatchEvent(client, hand);
        CFRelease(finger);
    }
    CFRelease(hand);
}

static void send_key(uint32_t usage, int down)
{
    IOHIDEventRef ev = IOHIDEventCreateKeyboardEvent(0, mach_absolute_time(),
            kHIDPage_Consumer, usage, down, 0);
    if (!ev) return;
    IOHIDEventSystemClientDispatchEvent(client, ev);
    CFRelease(ev);
}

static void apply(const insample_t *prev, const insample_t *cur)
{
    unsigned i;
    uint16_t diff = prev->buttons ^ cur->buttons;
    int was = prev->flags & INFLAG_PEN, is = cur->flags & INFLAG_PEN;
    float nx, ny;

    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (diff & keymap[i].btn) {
            if (opt_debug) say("[input] key %02x %s", keymap[i].usage,
                               (cur->buttons & keymap[i].btn) ? "down" : "up");
            send_key(keymap[i].usage, (cur->buttons & keymap[i].btn) != 0);
        }

    if (is && !was) {
        map_touch(cur, &nx, &ny);
        if (opt_debug) say("[input] down %d,%d", (int)(nx * 320), (int)(ny * 480));
        send_touch(nx, ny, 1, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
    } else if (is && (cur->tx != prev->tx || cur->ty != prev->ty)) {
        map_touch(cur, &nx, &ny);
        send_touch(nx, ny, 1, kIOHIDDigitizerEventPosition);
    } else if (!is && was) {
        map_touch(prev, &nx, &ny);          /* lift where the finger last was */
        if (opt_debug) say("[input] up %d,%d", (int)(nx * 320), (int)(ny * 480));
        send_touch(nx, ny, 0, kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch);
    }
}

static int client_alive(void)
{
    unsigned type = 0;
    mach_port_t p = CLIENT_PORT(client);
    if (!p) return 0;
    if (mach_port_type(mach_task_self_, p, &type) != 0) return 0;
    return !(type & MACH_PORT_TYPE_DEAD_NAME);
}

int main(int argc, char **argv)
{
    int mib[8], i;
    size_t miblen = 8, len;
    inmbox_t box;
    insample_t state;
    uint32_t tail = 0, idle = 0;
    int synced = 0;
    mach_port_t svc = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "flipx")) opt_flipx = 1;
        else if (!strcmp(argv[i], "flipy")) opt_flipy = 1;
        else if (!strcmp(argv[i], "stretch")) opt_stretch = 1;
        else if (!strcmp(argv[i], "debug")) opt_debug = 1;
    }

    /* the shim registers the sysctl at root mount; no sysctl = old shim */
    while (sysctlnametomib(INPUT_SYSCTL_NAME, mib, &miblen) != 0) {
        if (!idle++) say("[input] no %s (shim without input support); waiting", INPUT_SYSCTL_NAME);
        miblen = 8;
        sleep(10);
    }

    /* SpringBoard's launchd job owns the service; wait until it is known */
    while (bootstrap_look_up(bootstrap_port, "com.apple.iohideventsystem", &svc) != 0)
        sleep(1);
    mach_port_deallocate(mach_task_self_, svc);

    /* blocks in io_hideventsystem_open until SpringBoard serves the port */
    client = IOHIDEventSystemClientCreate(0);
    if (!client || !client_alive()) {
        say("[input] cannot open com.apple.iohideventsystem; retrying");
        sleep(2);
        return 1;                           /* launchd KeepAlive restarts us */
    }
    say("[input] connected to SpringBoard's HID event system");

    memset(&state, 0, sizeof state);
    for (idle = 0;; idle++) {
        len = sizeof box;
        if (sysctl(mib, (unsigned)miblen, &box, &len, 0, 0) != 0 || len != sizeof box ||
            box.magic != INPUT_MAGIC || box.version != INPUT_VERSION) {
            sleep(1);                       /* ARM9 without input support (yet) */
            continue;
        }
        if (!synced) {
            /* adopt the current state without replaying history, but never
               start with a finger or button "already down" */
            tail = box.head;
            synced = 1;
            say("[input] mailbox up: touch %s, flipx=%d flipy=%d stretch=%d",
                box.tsc_ok ? "ok" : "absent", opt_flipx, opt_flipy, opt_stretch);
        }
        if (box.head - tail >= INPUT_RING)  /* overrun: the oldest slot may be in flight */
            tail = box.head - (INPUT_RING - 1);
        while (tail != box.head) {
            const insample_t *s = &box.s[tail % INPUT_RING];
            apply(&state, s);
            state = *s;
            tail++;
        }
        if ((idle & 63) == 0 && !client_alive()) {
            say("[input] SpringBoard went away; restarting");
            return 0;                       /* reconnect via launchd */
        }
        usleep(16000);
    }
}
