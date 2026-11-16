/*
 * ios3ds ARM9 Wi-Fi glue: reads sd:/ios3ds/wifi.cfg, picks a backend and moves
 * Ethernet frames between it and the network mailbox that iOS polls.
 * See docs/wifi.md. Hooks into boot/arm9/main.c: wifi_init() after the SD is
 * mounted, wifi_poll() in the monitor loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"
#include "wpa2.h"
#include "log.h"

#define MBOX ((volatile netmbox_t *)NET_PA)
#define WIFI_CFG_PATH "0:/ios3ds/wifi.cfg"

static wifi_cfg_t cfg;
static const wifi_backend_t *be;
static u32 last_state = ~0u;
static char cfgtxt[1024];
static u8 frame[NET_FRAME_MAX] __attribute__((aligned(4)));

const u8 *ar6k_mac(void);

/* ---- loopback backend: frames sent by iOS come straight back -------------- */
static u8 loop_buf[NET_FRAME_MAX];
static u32 loop_len;
static int loop_init(const wifi_cfg_t *c, u8 mac[6])
{
    static const u8 def[6] = { 0x02, 0x33, 0x44, 0x53, 0x00, 0x01 };
    memcpy(mac, c->have_mac ? c->mac : def, 6);
    return 0;
}
static u32 loop_poll(void) { return NET_ST_UP; }
static int loop_tx(const u8 *eth, u32 len)
{
    if (loop_len) return -1;
    memcpy(loop_buf, eth, len);
    loop_len = len;
    return 0;
}
static u32 loop_rx(u8 *eth, u32 cap)
{
    u32 n = loop_len;
    if (!n || n > cap) return 0;
    memcpy(eth, loop_buf, n);
    loop_len = 0;
    return n;
}
static int loop_err(void) { return 0; }
const wifi_backend_t wifi_backend_loop = {
    .name = "loop", .id = NET_BACKEND_LOOP,
    .init = loop_init, .poll = loop_poll, .tx = loop_tx, .rx = loop_rx, .last_error = loop_err,
};

static const char *state_name(u32 s)
{
    static const char *const n[] = { "off", "power", "bmi", "fwload", "boot", "htc", "wmi",
                                     "scan", "auth", "up", "failed" };
    return s < ARRAY_SIZE(n) ? n[s] : "?";
}

void wifi_init(void)
{
    volatile netmbox_t *m = MBOX;
    u8 mac[6] = { 0 };
    be = NULL;
    /* Always initialise the mailbox: the kernel side then sees "served, no
       backend" instead of stale FCRAM. */
    net_mbox_init(m, NET_BACKEND_NONE, NULL);

    int n = sd_read_small(WIFI_CFG_PATH, cfgtxt, sizeof(cfgtxt));
    if (n < 0) {
        logp("[wifi] no %s: networking off\n", WIFI_CFG_PATH);
        return;
    }
    int r = wifi_cfg_parse(cfgtxt, &cfg);
    if (r < 0) {
        log_err("[wifi] wifi.cfg line %d is invalid: networking off\n", -r);
        return;
    }
    const char *why = wifi_cfg_check(&cfg);
    if (why) {
        log_err("[wifi] wifi.cfg: %s: networking off\n", why);
        return;
    }
    if (cfg.backend == WIFI_BE_OFF) {
        logp("[wifi] backend=off\n");
        return;
    }
    /* auto = real hardware. The emulator NIC is never probed implicitly. */
    const wifi_backend_t *want = cfg.backend == WIFI_BE_QEMU ? &wifi_backend_vnic
                               : cfg.backend == WIFI_BE_LOOP ? &wifi_backend_loop
                               : &wifi_backend_ar6014;
    if (want == &wifi_backend_ar6014 && cfg.security == WIFI_SEC_WPA2 && !cfg.have_pmk) {
        logp("[wifi] deriving the WPA2 key for \"%s\" (PBKDF2, takes a moment)...\n", cfg.ssid);
        wpa2_pmk(cfg.psk, (const u8 *)cfg.ssid, cfg.ssid_len, cfg.pmk);
        cfg.have_pmk = 1;
        memset(cfg.psk, 0, sizeof(cfg.psk));
    }
    if (want->init(&cfg, mac) != 0) {
        log_warn("[wifi] backend %s not available: networking off\n", want->name);
        m->err = want->last_error();
        m->state = NET_ST_FAILED;
        return;
    }
    be = want;
    net_mbox_init(m, be->id, mac);
    memcpy((void *)m->ssid, cfg.ssid, cfg.ssid_len);
    log_ok("[wifi] backend %s, mailbox at %08x\n", be->name, NET_PA);
}

bool wifi_poll(void)
{
    volatile netmbox_t *m = MBOX;
    bool busy = false;
    if (!be) return false;
    m->a9_polls++;

    u32 st = be->poll();
    if (st != last_state) {
        if (cfg.verbose || st == NET_ST_UP || st == NET_ST_FAILED)
            logp("[wifi] state %s -> %s\n", state_name(last_state), state_name(st));
        last_state = st;
        if (be == &wifi_backend_ar6014) {
            const u8 *mac = ar6k_mac();
            for (int i = 0; i < 6; i++) m->mac[i] = mac[i];
        }
        m->err = be->last_error();
        m->state = st;
        m->link = st == NET_ST_UP;
    }
    if (st != NET_ST_UP) {
        /* frames queued while the link is down would go out stale: drop them */
        while (net_mbox_peek_tx(m, frame, sizeof(frame))) net_mbox_tx_done(m);
        return false;
    }
    /* network -> iOS: bounded per call so block I/O keeps its latency */
    for (int i = 0; i < 4; i++) {
        u32 len = be->rx(frame, sizeof(frame));
        if (!len) break;
        net_mbox_push_rx(m, frame, len);
        if (cfg.verbose >= 2) logp("[wifi] rx %u\n", len);
        busy = true;
    }
    /* iOS -> network */
    for (int i = 0; i < 4; i++) {
        u32 len = net_mbox_peek_tx(m, frame, sizeof(frame));
        if (!len) break;
        if (be->tx(frame, len) == -1) break;        /* busy: retry next poll */
        net_mbox_tx_done(m);                        /* sent, or unsendable */
        if (cfg.verbose >= 2) logp("[wifi] tx %u\n", len);
        busy = true;
    }
    return busy;
}
