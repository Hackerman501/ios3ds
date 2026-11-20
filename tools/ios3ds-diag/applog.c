/*
 * ios3ds-applog.dylib: NSLog/stderr of the apps SpringBoard launches -> /dev/console.
 *
 * SpringBoard gives an app a stderr only for a debug launch. With the
 * SpringBoard default SBGuardMallocAlways it puts DYLD_INSERT_LIBRARIES=
 * /usr/lib/libgmalloc.dylib into every app's environment
 * (-[SBApplication _addInternalDebugVariablesToEnvironment], re-log 62); the
 * device has no libgmalloc, so the diag rootfs links that name to this library.
 * Active only when the kernel boot-args contain ios3ds_diag=1 (boot.cfg diag=1)
 * or ios3ds_applog=1 (added to boot.cfg's cmdline, without diag's test jobs):
 * then it sets CFLOG_FORCE_STDERR=1 (CFLog/NSLog also write to stderr) and
 * points stdout and stderr at /dev/console, and logs every alert's title and
 * text as "[applog] <program> alert ..." (UIModalView/UIAlertView setters and
 * popup). Otherwise it does nothing.
 *
 * Cydia Substrate (docs/cydia.md): every executable inside a .app bundle
 * (SpringBoard, which inserts us through its launchd plist, and every app, which
 * gets us as SpringBoard's libgmalloc.dylib) loads
 * /Library/MobileSubstrate/MobileSubstrate.dylib from here, before main(), as if
 * it had been inserted with it; without Substrate on the image nothing happens.
 * No launchd plist names Substrate: mobilesubstrate's extrainst_ strips it there.
 * Sandboxed apps cannot read /Library and stay without it (docs/cydia.md).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern const char *getprogname(void);
int unsetenv(const char *name);
int _NSGetExecutablePath(char *buf, unsigned int *size);

/* libobjc through dlsym: the stub SDK lists only the symbols the tools import */
typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef id (*IMP)(id, SEL, ...);
static id (*p_getClass)(const char *);
static Method (*p_getInstanceMethod)(id, SEL);
static IMP (*p_setImplementation)(Method, IMP);
static SEL (*p_sel)(const char *);
static id (*p_msgSend)(id, SEL, ...);

static void say(const char *what, id str)
{
    const char *c = "";
    if (str) c = (const char *)p_msgSend(str, p_sel("UTF8String"));
    char line[512];
    int l = snprintf(line, sizeof(line), "[applog] %s alert %s \"%s\"\n", getprogname(), what, c ? c : "");
    if (l > (int)sizeof(line) - 1) l = sizeof(line) - 1;
    write(2, line, l);
}

/* one trampoline per hooked method (a subclass may call its superclass' one) */
#define HOOK(n)                                                                  \
    static IMP orig##n;                                                          \
    static const char *what##n;                                                  \
    static id hook##n(id self, SEL cmd, id arg)                                  \
    {                                                                            \
        say(what##n, arg);                                                       \
        return orig##n(self, cmd, arg);                                          \
    }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5)

static IMP orig_popup;
static id hook_popup(id self, SEL cmd, int animated, int type, float x, float y)
{
    say("shown, title", p_msgSend(self, p_sel("title")));
    return ((id (*)(id, SEL, int, int, float, float))orig_popup)(self, cmd, animated, type, x, y);
}

/* -[UIViewController viewDidAppear:]: the controller's class and its view tree */
static IMP orig_appear;
static const char *(*p_className)(id);
static id hook_appear(id self, SEL cmd, int animated)
{
    id r = ((id (*)(id, SEL, int))orig_appear)(self, cmd, animated);
    id v = p_msgSend(self, p_sel("view"));
    id d = v ? p_msgSend(v, p_sel("recursiveDescription")) : 0;
    const char *c = d ? (const char *)p_msgSend(d, p_sel("UTF8String")) : 0;
    char line[160];
    int l = snprintf(line, sizeof(line), "[applog] %s appeared: %s\n", getprogname(),
                     p_className ? p_className(self) : "?");
    write(2, line, l);
    if (c) {
        size_t n = strlen(c);
        write(2, c, n > 16384 ? 16384 : n);
        write(2, "\n", 1);
    }
    return r;
}

static void hook_alerts(void)
{
    void *h = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!h) return;
    p_getClass = (id (*)(const char *))dlsym(h, "objc_getClass");
    p_getInstanceMethod = (Method (*)(id, SEL))dlsym(h, "class_getInstanceMethod");
    p_setImplementation = (IMP (*)(Method, IMP))dlsym(h, "method_setImplementation");
    p_sel = (SEL (*)(const char *))dlsym(h, "sel_registerName");
    p_msgSend = (id (*)(id, SEL, ...))dlsym(h, "objc_msgSend");
    if (!p_getClass || !p_getInstanceMethod || !p_setImplementation || !p_sel || !p_msgSend) return;
    static const char *names[6][2] = {
        {"UIModalView", "setTitle:"}, {"UIModalView", "setBodyText:"}, {"UIModalView", "setMessage:"},
        {"UIAlertView", "setTitle:"}, {"UIAlertView", "setBodyText:"}, {"UIAlertView", "setMessage:"},
    };
    IMP *origs[6] = {&orig0, &orig1, &orig2, &orig3, &orig4, &orig5};
    const char **whats[6] = {&what0, &what1, &what2, &what3, &what4, &what5};
    IMP hooks[6] = {(IMP)hook0, (IMP)hook1, (IMP)hook2, (IMP)hook3, (IMP)hook4, (IMP)hook5};
    Method done[6] = {0};
    for (int i = 0; i < 6; i++) {
        id cls = p_getClass(names[i][0]);
        Method m = cls ? p_getInstanceMethod(cls, p_sel(names[i][1])) : 0;
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= m && done[j] == m;
        if (!m || dup) continue;
        done[i] = m;
        *whats[i] = names[i][1];
        *origs[i] = p_setImplementation(m, hooks[i]);
    }
    id av = p_getClass("UIAlertView");
    Method m = av ? p_getInstanceMethod(av, p_sel("popupAlertAnimated:animationType:atOffset:")) : 0;
    if (m) orig_popup = p_setImplementation(m, (IMP)hook_popup);
    p_className = (const char *(*)(id))dlsym(h, "object_getClassName");
    id vc = p_getClass("UIViewController");
    m = vc ? p_getInstanceMethod(vc, p_sel("viewDidAppear:")) : 0;
    if (m) orig_appear = p_setImplementation(m, (IMP)hook_appear);
}

/*
 * Always on, in SpringBoard only: publish whether SpringBoard itself shows
 * something modal (lock screen, an alert item such as "Low Battery", or a
 * full-screen SBAlert such as the power-off slider) as the Darwin notify
 * state "com.ios3ds.sb.modal" (bit 0 locked, 1 alert item, 2 SBAlert). The
 * input daemon sends touches to the frontmost app's port, and the frontmost
 * app stays the app while SpringBoard's lock screen or an alert is over it:
 * touch "locked up" after a popup or a lock (issue #1, New 2DS XL log). The
 * daemon reads this state at every touch-down (tools/ios3ds-input).
 */
static int modal_token = -1, modal_last = -1;
static uint32_t (*p_notify_set_state)(int, uint64_t);

static int sb_responds(id o, const char *name)
{
    return o && p_msgSend(o, p_sel("respondsToSelector:"), p_sel(name)) != 0;
}

static id sb_call(id o, const char *name)
{
    return sb_responds(o, name) ? p_msgSend(o, p_sel(name)) : 0;
}

static void modal_tick(void *timer, void *info)
{
    (void)timer; (void)info;
    int m = 0;
    if (sb_call(sb_call(p_getClass("SBAwayController"), "sharedAwayController"), "isLocked")) m |= 1;
    if (sb_call(sb_call(p_getClass("SBAlertItemsController"), "sharedInstance"), "hasVisibleAlert")) m |= 2;
    id app = sb_call(p_getClass("UIApplication"), "sharedApplication");
    if (sb_call(app, "topMostAlert")) m |= 4;
    if (m != modal_last) {
        modal_last = m;
        p_notify_set_state(modal_token, (uint64_t)m);
    }
}

static void modal_start(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!objc || !cf) return;
    p_getClass = (id (*)(const char *))dlsym(objc, "objc_getClass");
    p_sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
    p_msgSend = (id (*)(id, SEL, ...))dlsym(objc, "objc_msgSend");
    uint32_t (*reg)(const char *, int *) = (uint32_t (*)(const char *, int *))dlsym(RTLD_DEFAULT, "notify_register_check");
    p_notify_set_state = (uint32_t (*)(int, uint64_t))dlsym(RTLD_DEFAULT, "notify_set_state");
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    double (*now)(void) = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **common = (void **)dlsym(cf, "kCFRunLoopCommonModes");
    if (!p_getClass || !p_sel || !p_msgSend || !reg || !p_notify_set_state || !mk || !now || !main_rl || !add || !common)
        return;
    if (reg("com.ios3ds.sb.modal", &modal_token) != 0) return;
    void *t = mk(NULL, now() + 1.0, 0.1, 0, 0, modal_tick, NULL);
    if (t) add(main_rl(), t, *common);
}

static void load_substrate(void)
{
    static const char ms[] = "/Library/MobileSubstrate/MobileSubstrate.dylib";
    const char *e = getenv("DYLD_INSERT_LIBRARIES");
    char exe[1024], old[1024], v[1200];
    unsigned int n = sizeof(exe);
    if (!e || strstr(e, ms) || strlen(e) >= sizeof(old)) return;   /* already inserted */
    if (_NSGetExecutablePath(exe, &n) != 0 || !strstr(exe, ".app/")) return;
    memcpy(old, e, strlen(e) + 1);
    snprintf(v, sizeof(v), "%s:%s", old, ms);
    /* Substrate's loader looks for itself in DYLD_INSERT_LIBRARIES and takes
     * itself out again, leaving ours. Sandboxed apps (MobileSafari, App Store
     * apps) may not read /Library: the dlopen fails and they run without it. */
    setenv("DYLD_INSERT_LIBRARIES", v, 1);
    if (!dlopen(ms, RTLD_LAZY | RTLD_GLOBAL)) setenv("DYLD_INSERT_LIBRARIES", old, 1);
}

__attribute__((constructor)) static void applog_init(void)
{
    if (!strcmp(getprogname(), "SpringBoard")) modal_start();
    load_substrate();
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, "ios3ds_diag=");
    const char *q = strstr(ba, "ios3ds_applog=");
    if (!(p && p[12] && p[12] != '0') && !(q && q[14] && q[14] != '0')) return;
    setenv("CFLOG_FORCE_STDERR", "1", 1);
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd < 0) return;
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    /* the app's children (and the next exec) need not inherit us */
    unsetenv("DYLD_INSERT_LIBRARIES");
    char line[160];
    int l = snprintf(line, sizeof(line), "[applog] %s pid %d%s%s\n", getprogname(), (int)getpid(),
                     dlopen("/Library/MobileSubstrate/MobileSubstrate.dylib", RTLD_LAZY | 0x10 /* NOLOAD */)
                         ? ", Substrate loaded" : "",
                     dlopen("/Library/MobileSubstrate/DynamicLibraries/tlsfix.dylib", RTLD_LAZY | 0x10)
                         ? ", TLSFix loaded" : "");
    write(2, line, l);
    hook_alerts();
}
