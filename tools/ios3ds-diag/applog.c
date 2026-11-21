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
 * popup), and MobileSafari's page-load errors with domain, code and userInfo.
 * Otherwise only the load errors (MobileSafari, re-log 75), SpringBoard's
 * modal state and SpringBoard's cache warm-up (below) are on.
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
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) write(2, line, l);
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
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) write(2, line, l);
    if (c) {
        size_t n = strlen(c);
        write(2, c, n > 16384 ? 16384 : n);
        write(2, "\n", 1);
    }
    return r;
}

/* MobileSafari's page-load failures (TabDocument, WebFrameLoadDelegate): the
   NSError behind "Cannot Open Page" with its domain, code and userInfo, whose
   _kCFStreamErrorDomainKey / _kCFStreamErrorCodeKey name the layer it came
   from (1 = POSIX socket call, 12 = getaddrinfo). The alert alone says only
   "Cannot allocate memory" (hardware reports, re-log 75). Always on in
   MobileSafari (one console line per failed load); the console is opened
   for these lines only unless applog is on. */
static IMP orig_fail[2];
static int errfd = 2, quiet_app;
static void say_error(const char *what, id err)
{
    char line[1024];
    id dom = err ? p_msgSend(err, p_sel("domain")) : 0;
    long code = err ? (long)p_msgSend(err, p_sel("code")) : 0;
    id info = err ? p_msgSend(err, p_sel("userInfo")) : 0;
    id desc = info ? p_msgSend(info, p_sel("description")) : 0;
    const char *d = dom ? (const char *)p_msgSend(dom, p_sel("UTF8String")) : "?";
    const char *i = desc ? (const char *)p_msgSend(desc, p_sel("UTF8String")) : "";
    int l = snprintf(line, sizeof(line), "[applog] %s load error (%s): %s %ld %s\n", getprogname(), what,
                     d ? d : "?", code, i ? i : "");
    if (l > (int)sizeof(line) - 1) l = sizeof(line) - 1;
    for (int k = 0; k < l - 1; k++)
        if (line[k] == '\n') line[k] = ' ';
    if (quiet_app && errfd == 2) {
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (fd < 0) return;
        errfd = fd;
    }
    write(errfd, line, l);
}

static id hook_fail0(id self, SEL cmd, id wv, id err, id frame);
static id hook_fail1(id self, SEL cmd, id wv, id err, id frame);
static void hook_load_errors(void)
{
    id td = p_getClass("TabDocument");
    Method m = td ? p_getInstanceMethod(td, p_sel("webView:didFailProvisionalLoadWithError:forFrame:")) : 0;
    if (m && !orig_fail[0]) orig_fail[0] = p_setImplementation(m, (IMP)hook_fail0);
    m = td ? p_getInstanceMethod(td, p_sel("webView:didFailLoadWithError:forFrame:")) : 0;
    if (m && !orig_fail[1]) orig_fail[1] = p_setImplementation(m, (IMP)hook_fail1);
}
static id hook_fail0(id self, SEL cmd, id wv, id err, id frame)
{
    say_error("provisional", err);
    return ((id (*)(id, SEL, id, id, id))orig_fail[0])(self, cmd, wv, err, frame);
}
static id hook_fail1(id self, SEL cmd, id wv, id err, id frame)
{
    say_error("load", err);
    return ((id (*)(id, SEL, id, id, id))orig_fail[1])(self, cmd, wv, err, frame);
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
    hook_load_errors();
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
    return o && ((uintptr_t)p_msgSend(o, p_sel("respondsToSelector:"), p_sel(name)) & 0xff) != 0;   /* BOOL */
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
    /* The power-off slider (and the lock screen) is an SBAlert in an
       SBAlertWindow, which topMostAlert does not see. An app stays frontmost
       while one of these windows covers it, so its touches must go to
       SpringBoard. Hardware run 27: the slider over Camera got nothing. */
    if (!(m & 4) && p_className) {
        id ws = sb_call(app, "windows");
        unsigned n = (ws && sb_responds(ws, "count")) ? (unsigned)(uintptr_t)p_msgSend(ws, p_sel("count")) : 0;
        if (n > 32) n = 32;
        for (unsigned i = 0; i < n && sb_responds(ws, "objectAtIndex:"); i++) {
            id w = p_msgSend(ws, p_sel("objectAtIndex:"), i);
            const char *cn = w ? p_className(w) : 0;
            if (cn && !strcmp(cn, "SBAlertWindow") &&
                !((uintptr_t)p_msgSend(w, p_sel("isHidden")) & 0xff)) {   /* BOOL in r0 */
                m |= 4;
                break;
            }
        }
    }
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
    p_className = (const char *(*)(id))dlsym(objc, "object_getClassName");
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

/*
 * SpringBoard warm-up (re-log 77). The first time a folder opens on a freshly
 * installed rootfs, SpringBoard stalls for ~2 s of emulated time (QEMU
 * --icount 2; several seconds on the console) before anything moves:
 *   - Folders' prepareToGhostCurrentPageIconsForRequester: asks every icon of
 *     the page for its "ghostly" (dimmed) image, -[SBIcon getIconImage:4];
 *     lsd renders each one (LICreateGhostlyVersion, ~25 ms of CPU per icon in
 *     QEMU, ~0.5 s for a page) and stores it in Caches/com.apple.IconsCache;
 *   - +[SBLinenView _generateLinenStripsIfNeeded] renders the folder's linen
 *     into Caches/com.apple.SpringBoard.folderSwitcherLinen.* (~0.15 s);
 *   - the first touch of an icon builds +[SBIcon homeScreenIconOverlayImage].
 * The two caches survive reboots, so after that only the first open per boot
 * pays for loading them. This does the same work in the background, one item
 * per main-runloop timer tick (0.5 s apart, from 30 s after SpringBoard
 * starts), so the first folder open finds everything ready. Boot-arg
 * ios3ds_sbwarm=0 turns it off.
 */
static int warm_step;
static id warm_icons;           /* retained NSArray of SBIconModel's leaf icons */
static unsigned warm_n, warm_done;
static double warm_t0, warm_busy;
static double (*p_now)(void);
static void (*p_invalidate)(void *);

static void warm_tick(void *timer, void *info)
{
    (void)info;
    double t = p_now();
    id ctl = sb_call(p_getClass("SBIconController"), "sharedInstance");
    id model = sb_call(p_getClass("SBIconModel"), "sharedInstance");
    if (!ctl || !model || ((uintptr_t)sb_call(ctl, "isEditing") & 0xff))   /* BOOL in r0 */
        return;                                         /* not up yet / user is busy */
    if (!warm_t0)
        warm_t0 = t;
    if (warm_step == 0) {
        sb_call(p_getClass("SBLinenView"), "_generateLinenStripsIfNeeded");
    } else if (warm_step == 1) {
        sb_call(p_getClass("SBIcon"), "homeScreenIconOverlayImage");
        sb_call(p_getClass("SBFolderIcon"), "homeScreenIconOverlayImage");
    } else if (warm_step == 2) {
        id set = sb_call(model, "leafIcons");
        id arr = sb_responds(set, "allObjects") ? sb_call(set, "allObjects") : set;
        warm_icons = arr && sb_responds(arr, "objectAtIndex:") ? sb_call(arr, "retain") : 0;
        warm_n = warm_icons ? (unsigned)(uintptr_t)sb_call(warm_icons, "count") : 0;
    } else if ((unsigned)warm_step - 3 < warm_n) {
        id icon = p_msgSend(warm_icons, p_sel("objectAtIndex:"), (unsigned)warm_step - 3);
        /* the image -[SBIcon prepareGhostlyImage] asks for (format 4) */
        if (sb_responds(icon, "getIconImage:") && !((uintptr_t)sb_call(icon, "isGhostly") & 0xff)) {
            p_msgSend(icon, p_sel("getIconImage:"), 4);
            warm_done++;
        }
    } else {
        char line[160];
        int l = snprintf(line, sizeof(line), "[sbwarm] linen, icon overlay and %u of %u ghostly icons ready "
                         "(%d ms on the main thread, done %d s after the first step)\n",
                         warm_done, warm_n, (int)(warm_busy * 1000), (int)(t - warm_t0));
        if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
        if (l > 0) write(2, line, l);
        if (warm_icons)
            sb_call(warm_icons, "release");
        warm_icons = 0;
        p_invalidate(timer);
        return;
    }
    warm_step++;
    warm_busy += p_now() - t;
}

static void warm_start(void)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) == 0) {
        ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
        if (strstr(ba, "ios3ds_sbwarm=0")) return;
    }
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf || !p_getClass || !p_sel || !p_msgSend) return;
    void *(*mk)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))
            dlsym(cf, "CFRunLoopTimerCreate");
    p_now = (double (*)(void))dlsym(cf, "CFAbsoluteTimeGetCurrent");
    p_invalidate = (void (*)(void *))dlsym(cf, "CFRunLoopTimerInvalidate");
    void *(*main_rl)(void) = (void *(*)(void))dlsym(cf, "CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(cf, "CFRunLoopAddTimer");
    void **defmode = (void **)dlsym(cf, "kCFRunLoopDefaultMode");
    if (!mk || !p_now || !p_invalidate || !main_rl || !add || !defmode) return;
    /* default mode only: no steps while a scroll/drag tracks touches */
    void *t = mk(NULL, p_now() + 30.0, 0.5, 0, 0, warm_tick, NULL);
    if (t) add(main_rl(), t, *defmode);
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
    if (!strcmp(getprogname(), "SpringBoard")) {
        modal_start();
        warm_start();
    }
    load_substrate();
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, "ios3ds_diag=");
    const char *q = strstr(ba, "ios3ds_applog=");
    if (!(p && p[12] && p[12] != '0') && !(q && q[14] && q[14] != '0')) {
        if (!strcmp(getprogname(), "MobileSafari")) {
            void *h = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
            if (!h) return;
            p_getClass = (id (*)(const char *))dlsym(h, "objc_getClass");
            p_getInstanceMethod = (Method (*)(id, SEL))dlsym(h, "class_getInstanceMethod");
            p_setImplementation = (IMP (*)(Method, IMP))dlsym(h, "method_setImplementation");
            p_sel = (SEL (*)(const char *))dlsym(h, "sel_registerName");
            p_msgSend = (id (*)(id, SEL, ...))dlsym(h, "objc_msgSend");
            if (!p_getClass || !p_getInstanceMethod || !p_setImplementation || !p_sel || !p_msgSend) return;
            quiet_app = 1;
            hook_load_errors();
        }
        return;
    }
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
    if (l > (int)sizeof(line) - 1) l = (int)sizeof(line) - 1;   /* snprintf returns the untruncated length */
    if (l > 0) write(2, line, l);
    hook_alerts();
}
