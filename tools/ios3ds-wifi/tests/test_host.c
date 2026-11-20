/*
 * Host unit tests for the pure parts of the Wi-Fi stack:
 *   - netmbox ring: ARM9 side (boot/arm9/wifi/wifi_ring.c) against the ARM11
 *     side (tools/ios3ds-wifi/shim/net_core.h) and the netio_t packing
 *   - Ethernet II <-> WMI data frame conversion (wifi_frame.c)
 *   - wifi.cfg parser (wifi_cfg.c)
 *   - WPA2: SHA-1, HMAC, PBKDF2, AES, RFC 3394 vectors, and a full 4-way +
 *     group handshake against a small authenticator written here (wpa2.c)
 * Build/run: make -C tools/ios3ds-wifi test
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"
#include "wpa2.h"

#define NET_DINVAL(p, len) ((void)(p), (void)(len))
#define NET_DCLEAN(p, len) ((void)(p), (void)(len))
#include "net_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void unhex(const char *h, u8 *out)
{
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (u8)v;
    }
}
static int eqhex(const u8 *b, const char *h)
{
    u8 t[256];
    unhex(h, t);
    return memcmp(b, t, strlen(h) / 2) == 0;
}

/* ---------------------------------------------------------------- ring */
static void mkframe(u8 *f, u32 len, u32 seed)
{
    for (u32 i = 0; i < len; i++) f[i] = (u8)(seed * 31 + i);
}

static void test_ring(void)
{
    static netmbox_t mb;
    static netio_t io;
    volatile netmbox_t *m = &mb;
    u8 f[NET_FRAME_MAX], g[NET_FRAME_MAX];
    const u8 mac[6] = { 2, 0, 0, 0, 0, 1 };

    memset(&mb, 0xEE, sizeof(mb));                  /* stale FCRAM */
    net_core_fill(m, &io);
    CHECK(io.magic == NETIO_MAGIC && io.link == NETIO_LINK_NONE);

    net_mbox_init(m, NET_BACKEND_LOOP, mac);
    CHECK(mb.magic == NET_MAGIC && mb.rx_head == 0 && mb.tx_tail == 0);
    /* the ARM11 line holds garbage (ARM9 restarted under a running iOS): the
       ARM9 refuses to push until the ARM11's next read resynchronises */
    mb.rx_tail = 0x12345678;
    CHECK(net_mbox_push_rx(m, f, 60) == -1);
    net_core_fill(m, &io);
    CHECK(mb.rx_tail == mb.rx_head && io.nframes == 0);
    mb.a9_rxdrop = 0;
    for (u32 i = 0; i < 3; i++) { mkframe(f, 60 + i, i); CHECK(net_mbox_push_rx(m, f, 60 + i) == 0); }
    net_core_fill(m, &io);
    CHECK(io.nframes == 3 && io.rx_next == 3 && io.rx_pending == 0 && io.link == 0);
    CHECK(memcmp(io.mac, mac, 6) == 0 && io.backend == NET_BACKEND_LOOP);
    CHECK(io.ssid[0] == 0 && io.nettest[0] == 0);
    memcpy((void *)mb.ssid, "net", 3);
    memset((void *)mb.nettest, 'x', sizeof(mb.nettest));    /* unterminated: clipped */
    net_core_fill(m, &io);
    CHECK(memcmp(io.ssid, "net", 4) == 0 && strlen(io.nettest) == 63);
    memset((void *)mb.nettest, 0, sizeof(mb.nettest));
    net_core_fill(m, &io);
    u32 off = 0, len = 0;
    for (u32 i = 0; i < 3; i++) {
        const u8 *p = netio_get(io.data, NETIO_DATA, &off, &len);
        mkframe(f, 60 + i, i);
        CHECK(p && len == 60 + i && memcmp(p, f, len) == 0);
    }
    CHECK(netio_get(io.data, NETIO_DATA, &off, &len) == NULL);    /* zero padding ends the list */
    /* a plain read does not consume */
    net_core_fill(m, &io);
    CHECK(io.nframes == 3);

    /* ack + 2 TX frames in one write */
    netio_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_next = io.rx_next;
    off = 0;
    mkframe(f, 1514, 7); CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 1514) == 0);
    mkframe(f, 99, 8);   CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 99) == 0);
    CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 13) == -1);    /* runt */
    cmd.nframes = 2;
    CHECK(net_core_apply(m, &cmd) == 2);
    CHECK(mb.rx_tail == 3 && mb.tx_head == 2);
    net_core_fill(m, &io);
    CHECK(io.nframes == 0 && io.tx_free == NET_SLOTS - 2);
    /* replaying the same ack is harmless; an ack beyond rx_head is ignored */
    cmd.nframes = 0; CHECK(net_core_apply(m, &cmd) == 0 && mb.rx_tail == 3);
    cmd.rx_next = 99; net_core_apply(m, &cmd); CHECK(mb.rx_tail == 3);

    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 1514);
    mkframe(f, 1514, 7); CHECK(memcmp(f, g, 1514) == 0);
    net_mbox_tx_done(m);
    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 99);
    net_mbox_tx_done(m);
    CHECK(net_mbox_peek_tx(m, g, sizeof(g)) == 0 && mb.a9_txframes == 2);

    /* RX ring full: the 17th frame is dropped and counted */
    for (u32 i = 0; i < NET_SLOTS; i++) CHECK(net_mbox_push_rx(m, f, 100) == 0);
    CHECK(net_mbox_push_rx(m, f, 100) == -1 && mb.a9_rxdrop == 1);
    CHECK(net_mbox_push_rx(m, f, 2000) == -2);
    /* a batch is limited by the 4 KiB io buffer, the rest stays pending */
    for (u32 i = 0; i < NET_SLOTS; i++) mb.rx[i].len = 1000;
    net_core_fill(m, &io);
    CHECK(io.nframes == 3 && io.rx_pending == NET_SLOTS - 3);   /* 3 x 1002 <= 3968 */

    /* wrap the 32-bit indices */
    net_mbox_init(m, NET_BACKEND_LOOP, mac);
    mb.rx_head = mb.rx_tail = 0xFFFFFFFEu;
    for (u32 i = 0; i < 5; i++) CHECK(net_mbox_push_rx(m, f, 64) == 0);
    net_core_fill(m, &io);
    CHECK(io.nframes == 5 && io.rx_next == 3);
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_next = io.rx_next;
    net_core_apply(m, &cmd);
    CHECK(mb.rx_tail == 3);
    /* TX ring full: extra frames are counted as dropped */
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = NETIO_CMD; cmd.version = NET_VERSION; cmd.rx_pending = NETIO_F_NOACK;
    off = 0;
    for (u32 i = 0; i < 18; i++) CHECK(netio_put(cmd.data, NETIO_DATA, &off, f, 60) == 0);
    cmd.nframes = 18;
    CHECK(net_core_apply(m, &cmd) == NET_SLOTS && mb.a11_txdrop == 2 && mb.rx_tail == 3);
}

/* ---------------------------------------------------------------- frames */
static void test_frames(void)
{
    u8 eth[1514], wmi[1600], back[1514];
    u16 et = 0;
    mkframe(eth, 1514, 3);
    eth[12] = 0x08; eth[13] = 0x00;
    u32 n = wifi_frame_to_wmi(eth, 1514, wmi, sizeof(wmi));
    CHECK(n == 2 + 14 + 8 + 1500);
    CHECK(wmi[0] == 0 && wmi[1] == 0x1C && wmi[14] == (1508 >> 8) && wmi[15] == (1508 & 0xFF));
    CHECK(eqhex(wmi + 16, "aaaa030000000800"));
    wmi[0] = 40; wmi[1] = 0;                      /* RX: rssi, 0 */
    CHECK(wifi_frame_from_wmi(wmi, n + 20, back, sizeof(back), &et) == 1514);  /* +padding */
    CHECK(et == 0x0800 && memcmp(back, eth, 1514) == 0);
    CHECK(wifi_frame_to_wmi(eth, 1514, wmi, 1523) == 0);          /* no room */
    CHECK(wifi_frame_to_wmi(eth, 1515, wmi, sizeof(wmi)) == 0);   /* too long */

    /* bridge-tunnel SNAP and an already-Ethernet-II frame */
    u8 rx[64] = { 0x30, 0 };
    memcpy(rx + 2, "\xff\xff\xff\xff\xff\xff\x02\x11\x22\x33\x44\x55", 12);
    rx[14] = 0; rx[15] = 8 + 4;
    memcpy(rx + 16, "\xaa\xaa\x03\x00\x00\xf8\x80\xf3" "ABCD", 12);
    n = wifi_frame_from_wmi(rx, 28, back, sizeof(back), &et);
    CHECK(n == 18 && et == 0x80F3 && back[12] == 0x80 && memcmp(back + 14, "ABCD", 4) == 0);
    rx[14] = 0x88; rx[15] = 0x8E;
    n = wifi_frame_from_wmi(rx, 30, back, sizeof(back), &et);
    CHECK(n == 28 && et == 0x888E);
    /* 802.3 length that lies about the payload */
    rx[14] = 0x05; rx[15] = 0x00;
    CHECK(wifi_frame_from_wmi(rx, 30, back, sizeof(back), &et) == 0);
    CHECK(wifi_frame_from_wmi(rx, 10, back, sizeof(back), &et) == 0);
}

/* ---------------------------------------------------------------- wlan */
static void test_wlan(void)
{
    static wlanmbox_t wb;
    static wlanio_t io, cmd;
    volatile wlanmbox_t *w = &wb;
    memset(&wb, 0xEE, sizeof(wb));                  /* stale FCRAM: not served */
    wlan_core_fill(w, &io);
    CHECK(io.magic == WLANIO_MAGIC && io.version == 0);
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = WLANIO_CMD; cmd.version = WLAN_VERSION; cmd.cmd.cmd = WLAN_CMD_SCAN;
    CHECK(wlan_core_apply(w, &cmd) == 0);           /* nobody serves: not posted */

    memset(&wb, 0, sizeof(wb));
    wb.st.magic = WLAN_MAGIC; wb.st.version = WLAN_VERSION; wb.st.state = WLAN_ST_IDLE;
    wb.st.nscan = 2; wb.bss[0].ssid_len = 3; memcpy(wb.bss[0].ssid, "abc", 3);
    wb.bss[1].rssi = -70; wb.bss[2].rssi = -1;      /* beyond nscan: not copied */
    wlan_core_fill(w, &io);
    CHECK(io.version == WLAN_VERSION && io.st.state == WLAN_ST_IDLE && io.st.nscan == 2);
    CHECK(memcmp(io.bss[0].ssid, "abc", 3) == 0 && io.bss[1].rssi == -70 && io.bss[2].rssi == 0);
    CHECK(io.cmd_seq == 0);
    cmd.cmd.cmd = WLAN_CMD_JOIN; cmd.cmd.ssid_len = 3; memcpy(cmd.cmd.ssid, "abc", 3);
    cmd.cmd.seq = 77;                               /* ignored: the shim numbers commands */
    CHECK(wlan_core_apply(w, &cmd) == 1 && wb.cmd.seq == 1 && wb.cmd.cmd == WLAN_CMD_JOIN);
    CHECK(wlan_core_apply(w, &cmd) == 1 && wb.cmd.seq == 2 && memcmp((const void *)wb.cmd.ssid, "abc", 3) == 0);
    wlan_core_fill(w, &io);
    CHECK(io.cmd_seq == 2);
    io.magic = WLANIO_MAGIC;                        /* a plain read written back: no command */
    CHECK(wlan_core_apply(w, &io) == 0 && wb.cmd.seq == 2);
}

/* ---------------------------------------------------------------- config */
static void test_cfg(void)
{
    wifi_cfg_t c;
    char t1[] = "# home\r\nssid=My Net \r\n psk = correct horse\nverbose=1\nunknown=1\n";
    CHECK(wifi_cfg_parse(t1, &c) == 0);
    CHECK(c.ssid_len == 7 && memcmp(c.ssid, "My Net ", 7) == 0);
    CHECK(strcmp(c.psk, " correct horse") == 0 && c.security == WIFI_SEC_WPA2 && c.verbose == 1);
    CHECK(c.backend == WIFI_BE_AUTO && strcmp(c.fw, "0:/ios3ds/wifi-fw.bin") == 0);
    CHECK(c.diag == 1 && c.fwmain == 4);                         /* defaults */
    CHECK(wifi_cfg_check(&c) == NULL);

    {
        char t[] = "diag=0\nfwmain=1\n", bad1[] = "diag=2\n", bad2[] = "fwmain=5\n";
        CHECK(wifi_cfg_parse(t, &c) == 0 && c.diag == 0 && c.fwmain == 1);
        CHECK(wifi_cfg_parse(bad1, &c) == -1 && wifi_cfg_parse(bad2, &c) == -1);
    }
    char t2[] = "ssid=x\npsk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n";
    CHECK(wifi_cfg_parse(t2, &c) == 0 && c.have_pmk && c.pmk[0] == 0xF4 && c.pmk[31] == 0x2E);

    char t3[] = "ssid=open\n";
    CHECK(wifi_cfg_parse(t3, &c) == 0 && c.security == WIFI_SEC_OPEN && wifi_cfg_check(&c) == NULL);
    char t4[] = "ssid=a\npsk=short\n";
    CHECK(wifi_cfg_parse(t4, &c) == -2);
    char t5[] = "security=wpa2\nssid=a\n";
    CHECK(wifi_cfg_parse(t5, &c) == 0 && wifi_cfg_check(&c) != NULL);
    char t6[] = "backend=qemu\nmac=03:00:00:00:00:01\n";
    CHECK(wifi_cfg_parse(t6, &c) == 0 && c.backend == WIFI_BE_QEMU && wifi_cfg_check(&c) != NULL);
    char t7[] = "backend=loop\nmac=02-00-00-00-00-01\n";
    CHECK(wifi_cfg_parse(t7, &c) == 0 && wifi_cfg_check(&c) == NULL && c.mac[5] == 1);
    char t8[] = "ssid=a\nnoequals\n";
    CHECK(wifi_cfg_parse(t8, &c) == -2);
    char t9[] = "ssid=123456789012345678901234567890123\n";
    CHECK(wifi_cfg_parse(t9, &c) == -1);
    char t10[] = "ssid=a\nnettest=http://10.0.2.2:8080/x example.com\n";
    CHECK(wifi_cfg_parse(t10, &c) == 0 && strcmp(c.nettest, "http://10.0.2.2:8080/x example.com") == 0);
    char t11[] = "ssid=a\nnettest=http://0123456789012345678901234567890123456789012345678901234567890123\n";
    CHECK(wifi_cfg_parse(t11, &c) == -2);
}

/* ---------------------------------------------------------------- crypto */
static void sha1(const void *p, u32 n, u8 out[20])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, p, n);
    sha1_final(&c, out);
}

static void aes_wrap(const u8 kek[16], const u8 *in, u32 n, u8 *out)   /* RFC 3394 */
{
    u8 a[8], b[16];
    u32 blocks = n / 8;
    memset(a, 0xA6, 8);
    memcpy(out + 8, in, n);
    for (u32 j = 0; j < 6; j++)
        for (u32 i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes128_encrypt(kek, b, b);
            u32 t = blocks * j + i;
            memcpy(a, b, 8);
            a[7] ^= (u8)t; a[6] ^= (u8)(t >> 8); a[5] ^= (u8)(t >> 16); a[4] ^= (u8)(t >> 24);
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

static void test_crypto(void)
{
    u8 d[64];
    sha1("abc", 3, d);
    CHECK(eqhex(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(m, (u32)strlen(m), d);
    CHECK(eqhex(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    static u8 big[1000000];
    memset(big, 'a', sizeof(big));
    sha1(big, sizeof(big), d);
    CHECK(eqhex(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

    hmac_ctx k;                                         /* RFC 2202 cases 2 and 6 */
    hmac_sha1_key(&k, (const u8 *)"Jefe", 4);
    hmac_sha1(&k, (const u8 *)"what do ya want ", 16, (const u8 *)"for nothing?", 12, d);
    CHECK(eqhex(d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    u8 key80[80];
    memset(key80, 0xAA, 80);
    hmac_sha1_key(&k, key80, 80);
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha1(&k, (const u8 *)msg, (u32)strlen(msg), NULL, 0, d);
    CHECK(eqhex(d, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));

    u8 pmk[32];                                         /* IEEE 802.11i H.4 */
    wpa2_pmk("password", (const u8 *)"IEEE", 4, pmk);
    CHECK(eqhex(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    wpa2_pmk("ThisIsAPassword", (const u8 *)"ThisIsASSID", 11, pmk);
    CHECK(eqhex(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));

    u8 key[16], pt[16], ct[16];                         /* FIPS-197 C.1 */
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    aes128_encrypt(key, pt, ct);
    CHECK(eqhex(ct, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(key, ct, d);
    CHECK(memcmp(d, pt, 16) == 0);

    u8 w[24], u[16];                                    /* RFC 3394 4.1 */
    unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", w);
    CHECK(aes_key_unwrap(key, w, 24, u) == 0 && eqhex(u, "00112233445566778899aabbccddeeff"));
    w[3] ^= 1;
    CHECK(aes_key_unwrap(key, w, 24, u) == -1);
    aes_wrap(key, pt, 16, w);
    CHECK(eqhex(w, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
}

/* ------------------------------------------------- 4-way handshake (AP side) */
#define EK_INFO 5
#define EK_REPLAY 9
#define EK_NONCE 17
#define EK_RSC 65
#define EK_MIC 81
#define EK_DATALEN 97
#define EK_DATA 99

static void ap_mic(const u8 kck[16], u8 *f, u32 len)
{
    hmac_ctx k;
    u8 d[20];
    memset(f + EK_MIC, 0, 16);
    hmac_sha1_key(&k, kck, 16);
    hmac_sha1(&k, f, len, NULL, 0, d);
    memcpy(f + EK_MIC, d, 16);
}

static u32 ap_frame(u8 *f, u32 info, u64 replay, const u8 *nonce, const u8 *kd, u32 kdlen)
{
    u32 total = EK_DATA + kdlen;
    memset(f, 0, total);
    f[0] = 2; f[1] = 3; f[2] = (u8)((total - 4) >> 8); f[3] = (u8)(total - 4);
    f[4] = 2; f[EK_INFO] = (u8)(info >> 8); f[EK_INFO + 1] = (u8)info;
    f[8] = 16;
    for (int i = 0; i < 8; i++) f[EK_REPLAY + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(f + EK_NONCE, nonce, 32);
    f[EK_DATALEN] = (u8)(kdlen >> 8); f[EK_DATALEN + 1] = (u8)kdlen;
    if (kdlen) memcpy(f + EK_DATA, kd, kdlen);
    return total;
}

static int mic_ok(const u8 kck[16], const u8 *f, u32 len)
{
    u8 c[256], save[16];
    memcpy(c, f, len);
    memcpy(save, c + EK_MIC, 16);
    ap_mic(kck, c, len);
    return memcmp(save, c + EK_MIC, 16) == 0;
}

static void test_handshake(void)
{
    wpa2_t w;
    u8 pmk[32], sta[6] = { 2, 1, 2, 3, 4, 5 }, ap[6] = { 0x10, 9, 8, 7, 6, 5 };
    u8 sn[32], an[32], ptk[48], out[256], f[256], gtk[16], kd[64], wrapped[72];
    u32 olen = 0;
    wpa2_pmk("correct horse battery", (const u8 *)"ios3ds", 6, pmk);
    for (int i = 0; i < 32; i++) { sn[i] = (u8)(i * 7); an[i] = (u8)(0x80 + i); }
    for (int i = 0; i < 16; i++) gtk[i] = (u8)(0xC0 + i);
    wpa2_start(&w, pmk, sta, ap, sn);
    wpa2_ptk(pmk, sta, ap, sn, an, ptk);

    /* M1 */
    u32 n = ap_frame(f, 0x008A, 1, an, NULL, 0);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_M2_SENT);
    CHECK(olen == EK_DATA + 22 && memcmp(out + EK_NONCE, sn, 32) == 0 && mic_ok(ptk, out, olen));
    CHECK(out[EK_INFO] == 0x01 && out[EK_INFO + 1] == 0x0A && out[EK_REPLAY + 7] == 1);
    CHECK(eqhex(out + EK_DATA, "30140100000fac040100000fac040100000fac020000"));

    /* M3: install, ack, MIC, secure, encrypted key data = RSN IE + GTK KDE + pad */
    u32 k = 0;
    memcpy(kd, out + EK_DATA, 22); k = 22;
    const u8 kde[8] = { 0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x01, 0x00 };
    memcpy(kd + k, kde, 8); k += 8;
    memcpy(kd + k, gtk, 16); k += 16;
    kd[k++] = 0xDD; while (k % 8) kd[k++] = 0;
    aes_wrap(ptk + 16, kd, k, wrapped);
    n = ap_frame(f, 0x13CA, 2, an, wrapped, k + 8);
    f[EK_RSC] = 0x42;
    ap_mic(ptk, f, n);
    u8 bad[256];
    memcpy(bad, f, n); bad[EK_MIC] ^= 1;
    CHECK(wpa2_rx_eapol(&w, bad, n, out, sizeof(out), &olen) == WPA2_ERR_MIC);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_KEYS);
    CHECK(memcmp(w.tk, ptk + 32, 16) == 0 && w.gtk_len == 16 && w.gtk_idx == 1);
    CHECK(memcmp(w.gtk, gtk, 16) == 0 && w.rsc[0] == 0x42);
    CHECK(olen == EK_DATA && mic_ok(ptk, out, olen) && out[EK_INFO] == 0x03 && out[EK_INFO + 1] == 0x0A);
    /* replayed M3 */
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_ERR_REPLAY);

    /* group rekey (group message 1), key id 2 */
    k = 0;
    const u8 kde2[8] = { 0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x02, 0x00 };
    memcpy(kd, kde2, 8); k = 8;
    for (int i = 0; i < 16; i++) kd[k++] = (u8)(0x50 + i);
    aes_wrap(ptk + 16, kd, k, wrapped);
    n = ap_frame(f, 0x1382, 3, NULL, wrapped, k + 8);
    ap_mic(ptk, f, n);
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_EV_GTK);
    CHECK(w.gtk_idx == 2 && w.gtk[0] == 0x50 && mic_ok(ptk, out, olen));
    CHECK(out[EK_INFO] == 0x03 && out[EK_INFO + 1] == 0x02);

    /* garbage */
    CHECK(wpa2_rx_eapol(&w, f, 50, out, sizeof(out), &olen) == WPA2_ERR_SHORT);
    f[1] = 0;
    CHECK(wpa2_rx_eapol(&w, f, n, out, sizeof(out), &olen) == WPA2_ERR_TYPE);
}

static void test_rsn_parse(void)
{
    rsn_info_t r;
    /* plain WPA2-PSK/CCMP (our own association IE) */
    static const u8 wpa2[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
    CHECK(wpa2_rsn_parse(wpa2, sizeof wpa2, &r) == RSN_MODE_WPA2 && r.ok && r.psk && !r.sae && r.group == 4);
    /* FRITZ!Box "WPA2 + WPA3": AKM PSK + SAE, MFPC, PMKID count 0, group mgmt BIP */
    static const u8 mixed[] = { 0x30, 0x1a, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                0x02, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x0F, 0xAC, 0x08, 0x80, 0x00,
                                0x00, 0x00 };
    CHECK(wpa2_rsn_parse(mixed, sizeof mixed, &r) == RSN_MODE_MIXED && r.psk && r.sae && r.mfpc && !r.mfpr && r.n_akm == 2);
    /* SAE listed first, TKIP group, group mgmt cipher present */
    static const u8 mixed2[] = { 0x30, 0x20, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                 0x00, 0x0F, 0xAC, 0x02, 0x02, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0x00, 0x0F, 0xAC, 0x02,
                                 0x80, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xAC, 0x06 };
    CHECK(wpa2_rsn_parse(mixed2, sizeof mixed2, &r) == RSN_MODE_MIXED && r.group == 2 && r.tkip && r.ccmp);
    /* WPA3 only: SAE, MFPC + MFPR */
    static const u8 wpa3[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                               0x01, 0x00, 0x00, 0x0F, 0xAC, 0x08, 0xc0, 0x00 };
    CHECK(wpa2_rsn_parse(wpa3, sizeof wpa3, &r) == RSN_MODE_WPA3_ONLY && r.mfpr);
    /* PSK but PMF required: we have no PMF, so not joinable either */
    static const u8 pmfreq[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                                 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0xc0, 0x00 };
    CHECK(wpa2_rsn_parse(pmfreq, sizeof pmfreq, &r) == RSN_MODE_WPA3_ONLY);
    /* 802.1X (enterprise) */
    static const u8 ent[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
                              0x01, 0x00, 0x00, 0x0F, 0xAC, 0x01, 0x00, 0x00 };
    CHECK(wpa2_rsn_parse(ent, sizeof ent, &r) == RSN_MODE_OTHER);
    /* truncated / garbage never reads past n */
    CHECK(wpa2_rsn_parse(mixed, 10, &r) == RSN_MODE_OTHER);
    static const u8 badcount[] = { 0x30, 0x0a, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0xff, 0x00, 0x00, 0x0F };
    CHECK(wpa2_rsn_parse(badcount, sizeof badcount, &r) == RSN_MODE_OTHER);
    static const u8 notrsn[] = { 0xdd, 0x04, 0x00, 0x50, 0xf2, 0x01 };
    CHECK(wpa2_rsn_parse(notrsn, sizeof notrsn, &r) == RSN_MODE_OTHER);
}

int main(void)
{
    test_ring();
    test_wlan();
    test_frames();
    test_cfg();
    test_crypto();
    test_handshake();
    test_rsn_parse();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
