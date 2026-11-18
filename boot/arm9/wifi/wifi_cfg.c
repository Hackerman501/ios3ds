/*
 * sd:/ios3ds/wifi.cfg parser. Pure C (no hardware, no logging): unit-tested on
 * the host by tools/ios3ds-wifi/tests.
 *
 *   # comment
 *   ssid=My Network          (everything after '=' up to end of line, 1-32 bytes)
 *   psk=correct horse        (8-63 characters, or 64 hex digits = the PMK itself)
 *   security=wpa2|open       (default: wpa2 when psk/pmk is given)
 *   backend=auto|ar6014|qemu|loop|off
 *   mac=02:11:22:33:44:55    (qemu/loop only; the AR6014 reports its own)
 *   fw=0:/ios3ds/wifi-fw.bin
 *   verbose=0|1|2
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* n bytes from 2n hex digits, optional single ':' or '-' between bytes */
static int parse_hex(const char *s, u8 *out, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (i && (*s == ':' || *s == '-')) s++;
        int hi = hexval(s[0]);
        if (hi < 0) return -1;
        int lo = hexval(s[1]);
        if (lo < 0) return -1;
        out[i] = (u8)(hi << 4 | lo);
        s += 2;
    }
    return *s ? -1 : 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static void copy_str(char *dst, const char *src, u32 size)
{
    u32 i = 0;
    for (; src[i] && i + 1 < size; i++) dst[i] = src[i];
    dst[i] = 0;
}

int wifi_cfg_parse(char *txt, wifi_cfg_t *c)
{
    int lineno = 0, sec_set = 0;
    memset(c, 0, sizeof(*c));
    copy_str(c->fw, "0:/ios3ds/wifi-fw.bin", sizeof(c->fw));
    char *line = txt;
    while (line && *line) {
        char *next = NULL;
        for (char *p = line; *p; p++) {
            if (*p == '\n') { *p = 0; next = p + 1; break; }
        }
        lineno++;
        u32 n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (!*eq) return -lineno;
            *eq = 0;
            char *k = line, *v = eq + 1;
            /* trim the key; the value is taken verbatim (SSIDs and
               passphrases may contain or end in spaces) */
            for (char *e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            u32 vl = strlen(v);
            if (streq(k, "ssid")) {
                if (vl < 1 || vl > 32) return -lineno;
                copy_str(c->ssid, v, sizeof(c->ssid));
                c->ssid_len = (u8)vl;
            } else if (streq(k, "psk") || streq(k, "pmk")) {
                if (vl == 64 && parse_hex(v, c->pmk, 32) == 0) {
                    c->have_pmk = 1;
                    c->psk[0] = 0;
                } else if (streq(k, "psk") && vl >= 8 && vl <= 63) {
                    copy_str(c->psk, v, sizeof(c->psk));
                    c->have_pmk = 0;
                } else {
                    return -lineno;
                }
            } else if (streq(k, "security")) {
                if (streq(v, "open")) c->security = WIFI_SEC_OPEN;
                else if (streq(v, "wpa2")) c->security = WIFI_SEC_WPA2;
                else return -lineno;
                sec_set = 1;
            } else if (streq(k, "backend")) {
                if (streq(v, "auto")) c->backend = WIFI_BE_AUTO;
                else if (streq(v, "off")) c->backend = WIFI_BE_OFF;
                else if (streq(v, "ar6014")) c->backend = WIFI_BE_AR6014;
                else if (streq(v, "qemu")) c->backend = WIFI_BE_QEMU;
                else if (streq(v, "loop")) c->backend = WIFI_BE_LOOP;
                else return -lineno;
            } else if (streq(k, "mac")) {
                if (parse_hex(v, c->mac, 6) != 0) return -lineno;
                c->have_mac = 1;
            } else if (streq(k, "fw")) {
                if (vl < 1 || vl >= sizeof(c->fw)) return -lineno;
                copy_str(c->fw, v, sizeof(c->fw));
            } else if (streq(k, "nettest")) {
                if (vl >= sizeof(c->nettest)) return -lineno;
                copy_str(c->nettest, v, sizeof(c->nettest));
            } else if (streq(k, "verbose")) {
                if (vl != 1 || v[0] < '0' || v[0] > '9') return -lineno;
                c->verbose = (u32)(v[0] - '0');
            }
            /* unknown keys: ignored, so newer configs work with older payloads */
        }
        line = next;
    }
    if (!sec_set) c->security = (c->have_pmk || c->psk[0]) ? WIFI_SEC_WPA2 : WIFI_SEC_OPEN;
    return 0;
}

const char *wifi_cfg_check(const wifi_cfg_t *c)
{
    if (c->backend == WIFI_BE_OFF) return NULL;
    int needs_ap = c->backend == WIFI_BE_AUTO || c->backend == WIFI_BE_AR6014;
    if (needs_ap && !c->ssid_len) return "ssid= missing";
    if (needs_ap && c->security == WIFI_SEC_WPA2 && !c->have_pmk && !c->psk[0])
        return "security=wpa2 needs psk=";
    if (c->have_mac && (c->mac[0] & 1)) return "mac= is a multicast address";
    return NULL;
}
