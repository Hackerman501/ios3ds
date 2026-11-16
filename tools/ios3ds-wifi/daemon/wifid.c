/*
 * ios3ds-wifid: bridge the ARM9's network mailbox to an iOS Ethernet interface.
 *
 *   ARM9 (AR6014 / QEMU vnic) --netmbox_t--> sysctl kern.ios3ds_net (shim)
 *     <--> this daemon <--> IOUserEthernet ("en0") <--> iOS TCP/IP, configd DHCP
 *
 * IOUserEthernet (com.apple.iokit.IOUserEthernet, prelinked at 0x80512000 in
 * the 8C148 kernelcache) publishes IOUserEthernetResource; the user-space
 * side is the IOEthernetController API in IOKit.framework (8C148 dyld cache):
 *   IOEthernetControllerCreate            0x33804bc4  (allocator, properties != NULL)
 *   IOEthernetControllerSetLinkStatus     0x33804a18  (ref, Boolean)
 *   IOEthernetControllerReadPacket        0x3380494c  = recv(CFSocketGetNative(ref+0x28), buf, len, 0)
 *   IOEthernetControllerWritePacket       0x338048d0
 *   IOEthernetControllerGetIONetworkInterfaceObject 0x33804a58
 *   kIOEthernetHardwareAddress            0x3e362824  (CFData, 6 bytes)
 * The kext only serves tasks with the entitlement
 * "com.apple.networking.ethernet.user-access" (ent.plist; signed by ios-cc).
 *
 * ReadPacket is a blocking recv() on the controller's socket, so we take the
 * descriptor ourselves (CFSocket at +0x28, verified above), make it
 * non-blocking and poll() it together with the mailbox.
 *
 * Addressing: on the first link-up we run "/usr/sbin/ipconfig set <if> DHCP"
 * (stock 8C148 tool) so configd's IPConfiguration agent does DHCP, routes
 * and DNS; "nodhcp" leaves that to SystemConfiguration preferences.
 *
 * Arguments: debug (log every frame), poll=<ms> (mailbox poll period,
 * default 4), nodhcp
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "wifi_proto.h"

/* ---- libSystem (stub SDK: declared here) ------------------------------ */
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int sysctlnametomib(const char *name, int *mibp, size_t *sizep);
struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd *fds, unsigned nfds, int timeout);
#define POLLIN 0x0001
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
#define EAGAIN_DARWIN 35
int waitpid(int pid, int *status, int options);
int execl(const char *path, const char *arg0, ...);

/* ---- CoreFoundation / IOKit ------------------------------------------ */
typedef const void *CFTypeRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFSocket *CFSocketRef;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef unsigned int io_object_t;
typedef int IOReturn;
typedef struct { CFIndex version; void *retain, *release, *copyDescription, *equal, *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; void *retain, *release, *copyDescription, *equal; } CFDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
CFDataRef CFDataCreate(CFAllocatorRef a, const uint8_t *bytes, CFIndex len);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef a, const void **keys, const void **values, CFIndex n,
                                   const CFDictionaryKeyCallBacks *kcb, const CFDictionaryValueCallBacks *vcb);
CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, unsigned enc);
Boolean CFStringGetCString(CFStringRef s, char *buf, CFIndex size, unsigned enc);
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t e, CFStringRef key, CFAllocatorRef a, unsigned opts);
int CFSocketGetNative(CFSocketRef s);
void CFRelease(CFTypeRef cf);
#define kCFStringEncodingUTF8 0x08000100u

typedef struct __IOEthernetController *IOEthernetControllerRef;
extern const CFStringRef kIOEthernetHardwareAddress;
IOEthernetControllerRef IOEthernetControllerCreate(CFAllocatorRef a, CFDictionaryRef props);
IOReturn IOEthernetControllerSetLinkStatus(IOEthernetControllerRef c, Boolean up);
CFIndex IOEthernetControllerReadPacket(IOEthernetControllerRef c, uint8_t *buf, CFIndex len);
IOReturn IOEthernetControllerWritePacket(IOEthernetControllerRef c, const uint8_t *buf, CFIndex len);
io_object_t IOEthernetControllerGetIONetworkInterfaceObject(IOEthernetControllerRef c);
#define CONTROLLER_SOCKET_OFF 0x28      /* IOKit 8C148: ReadPacket does ldr r0, [r0, #0x28] */

static int opt_debug, opt_poll_ms = 4, opt_dhcp = 1;
static char ifname[32] = "?";

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    int fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

static int mib[8];
static size_t miblen;
static netio_t io_in, io_out;

/* One exchange: io_out (ack + TX batch, or NULL for a plain read) in,
   fresh status + RX batch into io_in. */
static int xchg(const netio_t *out)
{
    size_t len = sizeof io_in;
    if (sysctl(mib, (unsigned)miblen, &io_in, &len, (void *)out, out ? sizeof *out : 0) != 0)
        return -1;
    return (len == sizeof io_in && io_in.magic == NETIO_MAGIC && io_in.version == NET_VERSION) ? 0 : -1;
}

static IOEthernetControllerRef make_controller(const uint8_t mac[6], int *fd)
{
    CFDataRef macd = CFDataCreate(kCFAllocatorDefault, mac, 6);
    const void *k[1] = { kIOEthernetHardwareAddress }, *v[1] = { macd };
    CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOEthernetControllerRef c = IOEthernetControllerCreate(kCFAllocatorDefault, props);
    CFRelease(props);
    CFRelease(macd);
    if (!c) return NULL;
    CFSocketRef s = *(CFSocketRef *)((char *)c + CONTROLLER_SOCKET_OFF);
    *fd = s ? CFSocketGetNative(s) : -1;
    if (*fd >= 0) fcntl(*fd, F_SETFL, fcntl(*fd, F_GETFL, 0) | O_NONBLOCK);

    char *name = ifname;
    io_object_t ifo = IOEthernetControllerGetIONetworkInterfaceObject(c);
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, "BSD Name", kCFStringEncodingUTF8);
    CFTypeRef bsd = ifo ? IORegistryEntryCreateCFProperty(ifo, key, kCFAllocatorDefault, 0) : NULL;
    if (bsd) {
        CFStringGetCString((CFStringRef)bsd, name, sizeof ifname, kCFStringEncodingUTF8);
        CFRelease(bsd);
    }
    CFRelease(key);
    say("[wifid] interface %s up, MAC %02x:%02x:%02x:%02x:%02x:%02x (socket fd %d)",
        name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], *fd);
    return c;
}

/* Ask configd's IPConfiguration agent to run DHCP on the new interface (the
   stock /usr/sbin/ipconfig; same as "ipconfig set en0 DHCP" on a Mac). */
static void start_dhcp(void)
{
    if (!opt_dhcp || ifname[0] == '?') return;
    int pid = fork();
    if (pid == 0) {
        execl("/usr/sbin/ipconfig", "ipconfig", "set", ifname, "DHCP", (char *)0);
        _exit(127);
    }
    int st = -1;
    if (pid > 0) waitpid(pid, &st, 0);
    say("[wifid] ipconfig set %s DHCP: status %d", ifname, st);
}

int main(int argc, char **argv)
{
    static uint8_t frame[2048];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "debug")) opt_debug = 1;
        else if (!strncmp(argv[i], "poll=", 5)) opt_poll_ms = atoi(argv[i] + 5);
        else if (!strcmp(argv[i], "nodhcp")) opt_dhcp = 0;
    }
    if (opt_poll_ms < 1) opt_poll_ms = 1;

    miblen = sizeof mib / sizeof mib[0];
    while (sysctlnametomib(NET_SYSCTL_NAME, mib, &miblen) != 0) {
        say("[wifid] %s missing (old shim?), retrying in 30 s", NET_SYSCTL_NAME);
        sleep(30);
        miblen = sizeof mib / sizeof mib[0];
    }

    /* wait until the ARM9 serves the mailbox and knows the MAC address */
    int quiet = 0;
    for (;;) {
        if (xchg(NULL) == 0 && io_in.link != NETIO_LINK_NONE && io_in.backend != NET_BACKEND_NONE &&
            (io_in.mac[0] | io_in.mac[1] | io_in.mac[2] | io_in.mac[3] | io_in.mac[4] | io_in.mac[5]))
            break;
        if (!quiet++) say("[wifid] waiting for the ARM9 network backend (state %u)", io_in.state);
        sleep(2);
    }
    int fd = -1;
    IOEthernetControllerRef ctl = make_controller(io_in.mac, &fd);
    if (!ctl || fd < 0) {
        say("[wifid] IOEthernetControllerCreate failed (entitlement? kext?)");
        return 1;
    }

    /* Per cycle: (1) write = ack of the batch we delivered + the TX frames
       iOS queued, (2) read = status + next RX batch. Two calls because the
       shim builds the outgoing batch before it applies the incoming ack
       (sysctl_handle_opaque copies out, then in), so a combined call would
       return the acknowledged frames once more. */
    int link = -1;
    uint32_t acked = io_in.rx_next, delivered = io_in.rx_next;
    for (;;) {
        uint32_t off = 0;
        uint16_t n = 0;
        memset(&io_out, 0, 32);
        io_out.magic = NETIO_CMD;
        io_out.version = NET_VERSION;
        while (NETIO_DATA - off >= 2 + NET_FRAME_MAX + 1) {        /* room for a full frame */
            CFIndex got = IOEthernetControllerReadPacket(ctl, frame, sizeof frame);
            if (got <= 0) break;                                    /* EAGAIN: queue empty */
            if (got > (CFIndex)NET_FRAME_MAX || netio_put(io_out.data, NETIO_DATA, &off, frame, (uint32_t)got) != 0)
                continue;                                           /* oversized / runt: drop */
            n++;
            if (opt_debug) say("[wifid] tx %ld", (long)got);
        }
        if (n || delivered != acked) {
            io_out.nframes = n;
            io_out.rx_next = delivered;
            if (sysctl(mib, (unsigned)miblen, NULL, NULL, &io_out, sizeof io_out) == 0) acked = delivered;
        }
        if (xchg(NULL) != 0) {
            sleep(1);
            continue;
        }
        if ((int)io_in.link != link) {
            link = io_in.link == 1;
            IOEthernetControllerSetLinkStatus(ctl, (Boolean)link);
            say("[wifid] link %s (ARM9 state %u)", link ? "up" : "down", io_in.state);
            static int dhcp_started;
            if (link && !dhcp_started) { dhcp_started = 1; start_dhcp(); }
        }
        off = 0;
        uint32_t len;
        const uint8_t *p;
        for (uint16_t i = 0; i < io_in.nframes && (p = netio_get(io_in.data, NETIO_DATA, &off, &len)); i++) {
            IOEthernetControllerWritePacket(ctl, p, (CFIndex)len);
            if (opt_debug) say("[wifid] rx %u", len);
        }
        delivered = io_in.rx_next;
        if (io_in.rx_pending || n || delivered != acked) continue;  /* more to do: go again now */

        /* idle: wait for iOS to send something, or the poll period */
        struct pollfd pfd = { fd, POLLIN, 0 };
        poll(&pfd, 1, opt_poll_ms);
    }
}
