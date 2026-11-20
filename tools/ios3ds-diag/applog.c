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
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern const char *getprogname(void);
int unsetenv(const char *name);

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

__attribute__((constructor)) static void applog_init(void)
{
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
    char line[96];
    int l = snprintf(line, sizeof(line), "[applog] %s pid %d\n", getprogname(), (int)getpid());
    write(2, line, l);
    hook_alerts();
}
