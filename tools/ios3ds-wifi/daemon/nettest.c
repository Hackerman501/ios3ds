/*
 * ios3ds-nettest: proves that iOS networking works over the Wi-Fi bridge.
 *
 *   ios3ds-nettest <interface> <url> [dns name]
 *
 * Started by ios3ds-wifid once the link is up when sd:/ios3ds/wifi.cfg has
 * "nettest=<url> [dns name]" (docs/wifi.md section 5). Everything goes
 * through the stock iOS stack: the address and routes come from configd's
 * DHCP client, names from mDNSResponder (getaddrinfo) with the DNS server
 * DHCP handed out, the fetch is a plain BSD-socket HTTP/1.0 GET.
 * Each step prints one "[nettest]" line to the console (sd:/ios3ds/log.txt):
 *
 *   [nettest] en0 address 10.0.2.15 netmask 255.255.255.0 (after 3 s)
 *   [nettest] default route via 10.0.2.2 (en0)
 *   [nettest] dns example.com -> 93.184.215.14
 *   [nettest] http 10.0.2.2:8080/path -> HTTP/1.0 200 OK | <first body line>
 *
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
#include <sys/types.h>

/* ---- libSystem (stub SDK: Darwin arm32 layouts declared here) --------- */
typedef uint32_t socklen_t;
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct in_addr { uint32_t s_addr; };
struct sockaddr_in { uint8_t sin_len, sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr, *ifa_netmask, *ifa_dstaddr;
    void *ifa_data;
};
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int socket(int, int, int);
int connect(int, const struct sockaddr *, socklen_t);
ssize_t send(int, const void *, size_t, int);
ssize_t recv(int, void *, size_t, int);
int setsockopt(int, int, int, const void *, socklen_t);
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
time_t time(time_t *);
size_t strcspn(const char *, const char *);
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xFFFF
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define CTL_NET 4
#define PF_ROUTE 17
#define NET_RT_DUMP 1
#define RTF_UP 0x1
#define RTF_GATEWAY 0x2
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define RTM_HDR 92                  /* sizeof(struct rt_msghdr), Darwin arm32 */
#define IFF_UP 0x1

static char ifname[32];

static void say(const char *fmt, ...)
{
    char line[300];
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

static const char *ip(uint32_t a)       /* network order */
{
    static char b[4][16];
    static int i;
    i = (i + 1) & 3;
    const uint8_t *p = (const uint8_t *)&a;
    snprintf(b[i], sizeof b[i], "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b[i];
}

/* the interface's IPv4 address, 0 = none yet */
static uint32_t if_addr(uint32_t *mask)
{
    struct ifaddrs *list = NULL, *i;
    uint32_t a = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, ifname)) {
            a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            if (mask && i->ifa_netmask) *mask = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
            break;
        }
    freeifaddrs(list);
    return a;
}

static void list_interfaces(void)
{
    struct ifaddrs *list = NULL, *i;
    char buf[256];
    int n = 0;
    if (getifaddrs(&list) != 0) return;
    buf[0] = 0;
    for (i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET)
            n += snprintf(buf + n, sizeof buf - n > 0 ? sizeof buf - n : 0, " %s=%s", i->ifa_name,
                          ip(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr));
    freeifaddrs(list);
    say("[nettest] IPv4 interfaces:%s", buf[0] ? buf : " none");
}

/* print the IPv4 default route(s) from the routing table */
static int default_route(void)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
    size_t len = 0;
    int found = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || !len) return 0;
    len += 1024;
    char *buf = malloc(len);
    if (!buf) return 0;
    if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) { free(buf); return 0; }
    for (char *p = buf; p + RTM_HDR <= buf + len; ) {
        uint16_t mlen = *(uint16_t *)p;
        if (mlen < RTM_HDR) break;
        int flags = *(int *)(p + 8), addrs = *(int *)(p + 12);
        uint16_t index = *(uint16_t *)(p + 4);
        struct sockaddr *sa = (struct sockaddr *)(p + RTM_HDR), *dst = NULL, *gw = NULL;
        for (int bit = 1; bit <= 0x80 && (char *)sa < p + mlen; bit <<= 1) {
            if (!(addrs & bit)) continue;
            if (bit == RTA_DST) dst = sa;
            if (bit == RTA_GATEWAY) gw = sa;
            int l = sa->sa_len ? (sa->sa_len + 3) & ~3 : 4;
            sa = (struct sockaddr *)((char *)sa + l);
        }
        if ((flags & RTF_GATEWAY) && (flags & RTF_UP) && dst && dst->sa_family == AF_INET &&
            ((struct sockaddr_in *)dst)->sin_addr.s_addr == 0 && gw && gw->sa_family == AF_INET) {
            say("[nettest] default route via %s (interface index %u)",
                ip(((struct sockaddr_in *)gw)->sin_addr.s_addr), index);
            found = 1;
        }
        p += mlen;
    }
    free(buf);
    return found;
}

static int resolve(const char *name, uint32_t *out)
{
    struct addrinfo hints, *res = NULL, *r;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(name, NULL, &hints, &res);
    if (e) {
        say("[nettest] dns %s failed: %s", name, gai_strerror(e));
        return -1;
    }
    char list[128];
    int n = 0;
    list[0] = 0;
    for (r = res; r; r = r->ai_next)
        if (r->ai_addr && r->ai_addr->sa_family == AF_INET) {
            uint32_t a = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
            if (!n) *out = a;
            n += snprintf(list + n, sizeof list - n > 0 ? sizeof list - n : 0, "%s%s", n ? " " : "", ip(a));
        }
    freeaddrinfo(res);
    say("[nettest] dns %s -> %s", name, n ? list : "(no IPv4 address)");
    return n ? 0 : -1;
}

static int http_get(const char *url)
{
    char host[64], path[128];
    unsigned port = 80;
    const char *p = url;
    if (strncmp(p, "http://", 7)) {
        say("[nettest] http: only http:// URLs (%s)", url);
        return -1;
    }
    p += 7;
    size_t hl = strcspn(p, ":/");
    if (hl == 0 || hl >= sizeof host) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;
    if (*p == ':') port = (unsigned)strtoul(p + 1, (char **)&p, 10);
    snprintf(path, sizeof path, "%s", *p ? p : "/");

    uint32_t a = 0;
    if (resolve(host, &a) != 0) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { say("[nettest] http: socket: %d", errno); return -1; }
    struct timeval tv = { 10, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_len = sizeof sin;
    sin.sin_family = AF_INET;
    sin.sin_port = (uint16_t)(port >> 8 | (port & 0xFF) << 8);
    sin.sin_addr.s_addr = a;
    time_t t0 = time(NULL);
    if (connect(s, (struct sockaddr *)&sin, sizeof sin) != 0) {
        say("[nettest] http %s:%u: connect failed, errno %d", ip(a), port, errno);
        close(s);
        return -1;
    }
    char req[256], resp[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ios3ds-nettest\r\n\r\n",
                     path, host);
    send(s, req, (size_t)n, 0);
    int got = 0;
    for (;;) {
        ssize_t r = recv(s, resp + got, sizeof resp - 1 - got, 0);
        if (r <= 0) break;
        got += (int)r;
        if (got >= (int)sizeof resp - 1) break;
    }
    close(s);
    resp[got > 0 ? got : 0] = 0;
    if (got <= 0) {
        say("[nettest] http %s:%u%s: no answer (errno %d)", ip(a), port, path, errno);
        return -1;
    }
    char *eol = strstr(resp, "\r\n"), *body = strstr(resp, "\r\n\r\n");
    if (eol) *eol = 0;
    if (body) {
        body += 4;
        body[strcspn(body, "\r\n")] = 0;
    }
    say("[nettest] http %s:%u%s -> %s | %s (%d bytes, %ld s)", ip(a), port, path, resp,
        body ? body : "", got, (long)(time(NULL) - t0));
    return strstr(resp, " 200 ") ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        say("[nettest] usage: ios3ds-nettest <interface> <url> [dns name]");
        return 2;
    }
    snprintf(ifname, sizeof ifname, "%s", argv[1]);
    say("[nettest] waiting for an IPv4 address on %s (DHCP)", ifname);
    uint32_t a = 0, mask = 0;
    time_t t0 = time(NULL);
    for (int i = 0; i < 120 && !(a = if_addr(&mask)); i++) sleep(1);
    if (!a) {
        say("[nettest] %s: no address after %ld s", ifname, (long)(time(NULL) - t0));
        list_interfaces();
        return 1;
    }
    say("[nettest] %s address %s netmask %s (after %ld s)", ifname, ip(a), ip(mask),
        (long)(time(NULL) - t0));
    list_interfaces();
    int rc = 0;
    /* the router and DNS come a moment after the address (IPMonitor) */
    int route = 0;
    for (int i = 0; i < 20 && !(route = default_route()); i++) sleep(1);
    if (!route) {
        say("[nettest] no IPv4 default route");
        rc = 1;
    }
    if (argc > 3) {
        uint32_t d = 0;
        int ok = -1;
        for (int i = 0; i < 5 && ok != 0; i++) {
            if (i) sleep(3);
            ok = resolve(argv[3], &d);
        }
        if (ok != 0) rc = 1;
    }
    if (http_get(argv[2]) != 0) rc = 1;
    say("[nettest] %s", rc ? "FAILED" : "all steps passed");
    return rc;
}
