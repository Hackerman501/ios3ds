/*
 * ios3ds-diag: launchd wrapper for the test and diagnostic jobs (gltest, the
 * boot tone, the [smp] sampler). Runs "argv[1] argv[2..]" only when the
 * kernel boot-args contain ios3ds_diag=1 (boot.cfg diag=1 adds it), else
 * exits 0 at once. docs/boot-perf.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <dlfcn.h>

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

/* "ios3ds-diag -iobusy [secs]": list the IOServices whose busy count is not 0,
   i.e. what launchctl's IOKitWaitQuiet (5 s timeout at boot) waits for.
   IOKit is looked up with dlsym: the stub SDK lists only the symbols the
   other tools import. */
typedef unsigned int io_obj;
static int iobusy(int secs)
{
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!h) return 1;
    void *(*matching)(const char *) = (void *(*)(const char *))dlsym(h, "IOServiceMatching");
    int (*get)(unsigned, void *, io_obj *) = (int (*)(unsigned, void *, io_obj *))dlsym(h, "IOServiceGetMatchingServices");
    io_obj (*next)(io_obj) = (io_obj (*)(io_obj))dlsym(h, "IOIteratorNext");
    int (*busy)(io_obj, unsigned *) = (int (*)(io_obj, unsigned *))dlsym(h, "IOServiceGetBusyState");
    int (*name)(io_obj, char *) = (int (*)(io_obj, char *))dlsym(h, "IORegistryEntryGetName");
    int (*cls)(io_obj, char *) = (int (*)(io_obj, char *))dlsym(h, "IOObjectGetClass");
    int (*rel)(io_obj) = (int (*)(io_obj))dlsym(h, "IOObjectRelease");
    io_obj (*root)(unsigned) = (io_obj (*)(unsigned))dlsym(h, "IORegistryGetRootEntry");
    int (*iter)(io_obj, const char *, unsigned, io_obj *) =
        (int (*)(io_obj, const char *, unsigned, io_obj *))dlsym(h, "IORegistryEntryCreateIterator");
    int (*state)(io_obj, unsigned long long *) = (int (*)(io_obj, unsigned long long *))dlsym(h, "IOServiceGetState");
    if (!matching || !get || !next || !busy || !name || !cls || !rel) return 1;
    for (int pass = 0; pass <= secs; pass++) {
        io_obj it = 0, s;
        /* the whole IOService plane (also services not registered yet), else matching */
        if (!(root && iter && iter(root(0), "IOService", 1, &it) == 0) &&
            get(0, matching("IOService"), &it) != 0) return 1;
        int total = 0, nbusy = 0;
        while ((s = next(it)) != 0) {
            unsigned b = 0;
            char n[128] = "", c[128] = "";
            total++;
            if (busy(s, &b) == 0 && b) {
                name(s, n);
                cls(s, c);
                unsigned long long st = 0;
                if (state) state(s, &st);
                printf("[iobusy] %ds: %s (%s) busy %u state %llx\n", pass, n, c, b, st);
                nbusy++;
            }
            rel(s);
        }
        rel(it);
        printf("[iobusy] %ds: %d of %d services busy\n", pass, nbusy, total);
        fflush(stdout);
        if (!nbusy) break;
        sleep(1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    char ba[512];
    size_t n = sizeof(ba) - 1;
    if (argc >= 2 && !strcmp(argv[1], "-iobusy")) return iobusy(argc >= 3 ? atoi(argv[2]) : 0);
    if (argc < 2) {
        fprintf(stderr, "usage: ios3ds-diag program [args...]\n");
        return 2;
    }
    if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0) != 0) return 0;
    ba[n < sizeof(ba) ? n : sizeof(ba) - 1] = 0;
    const char *p = strstr(ba, "ios3ds_diag=");
    if (!p || p[12] == '0' || p[12] == 0) return 0;
    execv(argv[1], argv + 1);
    perror(argv[1]);
    return 1;
}
