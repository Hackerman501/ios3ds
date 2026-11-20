/*
 * ios3ds-gyrotest: does CoreMotion deliver the 3DS gyroscope? (docs/mcu.md 11)
 *
 * A diag job (boot.cfg diag=1, run through ios3ds-diag). Waits for
 * SpringBoard's HID event system, then uses CMMotionManager exactly as an
 * iPhone 4 app would (pull mode): gyroAvailable / deviceMotionAvailable,
 * startGyroUpdates + startDeviceMotionUpdates, and prints gyroData.rotationRate
 * and deviceMotion's rotation rate and attitude once a second for `secs`
 * seconds; then stops the updates (the gyro goes back to sleep) and prints a
 * summary line:
 *   [gyrotest] gyroAvailable 1 deviceMotionAvailable 1
 *   [gyrotest] t=3 gyro 0.000 0.524 0.000 rad/s | dm rate ... | attitude ...
 *   [gyrotest] done: N gyro samples changed, last 0.000 0.524 0.000
 * Usage: ios3ds-gyrotest [secs (20)] [interval ms (20)]
 * Objective-C through objc_msgSend from C (the stub SDK has no ObjC headers).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t bootstrap_port;
kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);

typedef void *id;
typedef void *SEL;
extern id objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern id objc_msgSend(id, SEL, ...);
extern void objc_msgSend_stret(void *, id, SEL, ...);

typedef struct { double x, y, z; } vec3;       /* CMRotationRate */

#define MSG(type, obj, sel) ((type)objc_msgSend)(obj, sel_registerName(sel)
static id obj0(id o, const char *sel) { return o ? MSG(id (*)(id, SEL), o, sel)) : 0; }
static int bool0(id o, const char *sel) { return MSG(signed char (*)(id, SEL), o, sel)); }
static void void0(id o, const char *sel) { MSG(void (*)(id, SEL), o, sel)); }
static double dbl0(id o, const char *sel) { return o ? MSG(double (*)(id, SEL), o, sel)) : 0; }

static vec3 rate(id obj)
{
    vec3 v = { 0, 0, 0 };
    if (obj) ((void (*)(vec3 *, id, SEL))objc_msgSend_stret)(&v, obj, sel_registerName("rotationRate"));
    return v;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    int ms = argc > 2 ? atoi(argv[2]) : 20;
    setvbuf(stdout, NULL, 1 /* _IOLBF */, 0);

    /* CoreMotion talks to SpringBoard's IOHIDEventSystem: wait for it */
    mach_port_t svc = 0;
    for (int i = 0; bootstrap_look_up(bootstrap_port, "com.apple.iohideventsystem", &svc) != 0; i++) {
        if (i == 600) { printf("[gyrotest] no SpringBoard HID event system after 10 min\n"); return 1; }
        sleep(1);
    }
    sleep(5);
    if (!dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW)) {
        printf("[gyrotest] dlopen CoreMotion: %s\n", dlerror());
        return 1;
    }
    id pool = obj0(obj0(objc_getClass("NSAutoreleasePool"), "alloc"), "init");
    id mm = obj0(obj0(objc_getClass("CMMotionManager"), "alloc"), "init");
    if (!mm) { printf("[gyrotest] no CMMotionManager\n"); return 1; }
    int ga = bool0(mm, "isGyroAvailable");
    int da = bool0(mm, "isDeviceMotionAvailable");
    int aa = bool0(mm, "isAccelerometerAvailable");
    printf("[gyrotest] gyroAvailable %d deviceMotionAvailable %d accelerometerAvailable %d\n", ga, da, aa);
    if (!ga) return 1;

    MSG(void (*)(id, SEL, double), mm, "setGyroUpdateInterval:"), ms / 1000.0);
    MSG(void (*)(id, SEL, double), mm, "setDeviceMotionUpdateInterval:"), ms / 1000.0);
    void0(mm, "startGyroUpdates");
    if (da) void0(mm, "startDeviceMotionUpdates");
    printf("[gyrotest] started (interval %d ms): gyroActive %d deviceMotionActive %d\n", ms,
           bool0(mm, "isGyroActive"),
           bool0(mm, "isDeviceMotionActive"));

    vec3 last = { 0, 0, 0 };
    int changed = 0, nonzero = 0;
    for (int t = 1; t <= secs; t++) {
        for (int i = 0; i < 10; i++) {           /* sample 10x a second, print once */
            usleep(100000);
            vec3 g = rate(obj0(mm, "gyroData"));
            if (g.x != last.x || g.y != last.y || g.z != last.z) changed++;
            if (g.x != 0 || g.y != 0 || g.z != 0) nonzero++;
            last = g;
        }
        id dm = da ? obj0(mm, "deviceMotion") : 0;
        vec3 r = rate(dm);
        id att = obj0(dm, "attitude");
        double roll = dbl0(att, "roll");
        double pitch = dbl0(att, "pitch");
        double yaw = dbl0(att, "yaw");
        printf("[gyrotest] t=%d gyro %.3f %.3f %.3f rad/s | deviceMotion rate %.3f %.3f %.3f | "
               "attitude roll %.2f pitch %.2f yaw %.2f\n",
               t, last.x, last.y, last.z, r.x, r.y, r.z, roll, pitch, yaw);
    }
    void0(mm, "stopGyroUpdates");
    if (da) void0(mm, "stopDeviceMotionUpdates");
    printf("[gyrotest] done: gyro value changed %d times, %d non-zero reads, last %.3f %.3f %.3f; updates stopped\n",
           changed, nonzero, last.x, last.y, last.z);
    (void)pool;
    sleep(1000000);         /* stay (KeepAlive false; avoids a respawn loop) */
    return 0;
}
