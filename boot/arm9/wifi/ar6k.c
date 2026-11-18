/*
 * Atheros AR6014 ("DWM-W028", the Old 3DS Wi-Fi chip) bring-up on the ARM9:
 * power, SDIO, BMI bootloader, firmware upload, HTC/WMI handshake, connect,
 * WPA2 4-way handshake, 802.3 data path.
 *
 * STATUS: SKELETON, NEVER RUN ON HARDWARE. It compiles, every wait is bounded
 * and every step logs, so the first console runs tell us where it stops.
 * Sources for each step are in docs/wifi.md section 2-4 (GBATEK "DSi Atheros
 * Wifi", dsiwifi (MIT, (c) 2021 Max Thomas) for the SDIO/BMI/WMI sequences,
 * Linux ath6kl for the WMI structures).
 *
 * The chip has no usable ROM-only mode: BMI_DONE jumps to RAM (0x927000), so
 * the console's own firmware parts must be uploaded first. They come from
 * sd:/ios3ds/wifi-fw.bin, which tools/ios3ds-wifi/nwmfw.py cuts out of the
 * user's own NWM system module (GBATEK "3DS Files - Module NWM"). Nothing of
 * it is in this repository.
 *
 * Power-up follows libn3ds WIFI_init (CFG11_WIFICNT, GPIO 2_2 = SDIO select,
 * GPIO 3_12 = nRESET, ~2 ms); the BMI register setup before the upload and the
 * HTC service list follow dsiwifi wifi_card_wlan_init / htc_handle_pkt.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"
#include "sdio.h"
#include "wpa2.h"
#include "log.h"

void waitcycles(u32 n);

/* ---- 3DS-side power and reset ------------------------------------------ */
#define CFG11_WIFICNT   (*(vu8 *)0x10140180u)   /* bit0: enable the Wi-Fi subsystem */
#define GPIO_WIFI_MODE  (*(vu16 *)0x10147014u)  /* GPIO 2_2 (REG_GPIO2_DAT2): 0 = SDIO/Atheros, 1 = DS radio */
#define GPIO_WIFI_EN    (*(vu16 *)0x10147028u)  /* GPIO 3_12 (REG_GPIO3_DAT2): Atheros nRESET, 1 = run */
#define REG_PRNG        (*(vu32 *)0x10011000u)  /* ARM9 PRNG source */
#define REG_TIMER0      (*(vu16 *)0x10003000u)
/* the monitor's free-running clock (boot/arm9/main.c clk_start(): timers 2+3
   cascaded, prescaler 1024 = 65457 Hz), running before wifi_poll() is called */
#define REG_T9_VAL2     (*(vu16 *)0x10003008u)
#define REG_T9_VAL3     (*(vu16 *)0x1000300Cu)

/* ---- AR6014 (GBATEK "Internal Memory Map") ------------------------------- */
#define CHIP_ID_AR6014      0x0D000001u
#define HOST_INTEREST       0x00520000u
#define HI_DSET_LIST_HEAD   0x18
#define HI_BOARD_DATA_INIT  0x58
#define HI_MBOX_IO_BLOCK_SZ 0x6C
#define HI_MBOX_ISR_YIELD   0x74
#define SOC_RESET_CONTROL   0x00004000u     /* bit8: warm reset into the BMI ROM */
#define SOC_CHIP_ID         0x000040ECu
#define SOC_WLAN_SYS_SLEEP  0x000040C4u
#define SOC_CLOCK_CONTROL   0x00004028u
#define SOC_REG_4020        0x00004020u
#define SOC_SCRATCH0        0x000180C0u
/* function 1 registers */
#define F1_HOST_INT_STATUS  0x400
#define F1_RX_LOOKAHEAD_VALID 0x405
#define F1_RX_LOOKAHEAD0    0x408
#define F1_INT_STATUS_EN    0x418
#define F1_SCRATCH0         0x460
#define F1_COUNT4           0x450
#define F1_WINDOW_DATA      0x474
#define F1_WINDOW_WRITE_ADDR 0x478
#define F1_WINDOW_READ_ADDR 0x47C
#define F1_MBOX0_BYTE       0xFF            /* any address in the MBOX0 window */
#define F1_MBOX0_END        0x4000          /* block transfers end here */

enum { BMI_DONE = 1, BMI_WRITE_MEMORY = 3, BMI_EXECUTE = 4, BMI_READ_SOC_REG = 6,
       BMI_WRITE_SOC_REG = 7, BMI_GET_TARGET_ID = 8, BMI_LZ_START = 0xD, BMI_LZ_DATA = 0xE };

/* HTC endpoint 0 is the control endpoint; the others are handed out by the
   firmware in CONNECT_SERVICE responses (ath6kl htc_conn_service_resp), in
   practice 1 = WMI control, 2..5 = data BE/BK/VI/VO */
enum { PKT_HTC = 0 };
#define HTC_FLG_RX_TRAILER 0x02
enum { HTC_READY = 1, HTC_CONN_SVC = 2, HTC_SETUP_COMPLETE = 4 };
enum { WMI_CONNECT = 0x0001, WMI_DISCONNECT = 0x0003, WMI_SYNCHRONIZE = 0x0004,
       WMI_START_SCAN = 0x0007, WMI_SET_BSS_FILTER = 0x0009, WMI_SET_PROBED_SSID = 0x000A,
       WMI_SET_DISC_TIMEOUT = 0x000D, WMI_SET_POWER_MODE = 0x0012,
       WMI_ADD_CIPHER_KEY = 0x0016, WMI_TARGET_ERROR_BITMASK = 0x0022,
       WMI_SET_KEEPALIVE = 0x003D, WMI_SET_HEARTBEAT_TIMEOUT = 0x0047,
       WMI_EV_READY = 0x1001, WMI_EV_CONNECT = 0x1002, WMI_EV_DISCONNECT = 0x1003,
       WMI_EV_BSSINFO = 0x1004, WMI_EV_SCAN_COMPLETE = 0x100A };
/* WMI crypto types as the DSi/3DS firmware uses them (GBATEK "WMI Connect
   Functions": NONE 1, WEP 2, TKIP 3, AES 4 - not the ath6kl bit values) */
enum { CRYPT_NONE = 1, CRYPT_TKIP = 3, CRYPT_AES = 4 };
enum { AUTH_NONE = 1, AUTH_WPA2_PSK = 5 };  /* WMI authMode as sent by dsiwifi */

/* ---- sd:/ios3ds/wifi-fw.bin (written by tools/ios3ds-wifi/nwmfw.py) ------ */
#define FW_MAGIC 0x46573349u    /* 'I3WF' */
enum { FW_STUB_DATA = 1, FW_STUB_CODE = 2, FW_DATABASE = 3, FW_MAIN_LZ = 4 };
typedef struct { u32 kind, dst, off, len; } fw_part_t;
typedef struct { u32 magic, version, nparts, chip_id; fw_part_t part[8]; } fw_hdr_t;
/* Main.type4 is up to 0xA5EB bytes (GBATEK); 64 KiB of ARM9 RAM, used only
   until the upload is done */
static u8 fw_buf[64 * 1024] __attribute__((aligned(4)));
static u32 fw_size;

/* ---- driver state -------------------------------------------------------- */
static struct {
    u32 state;
    int err;
    const wifi_cfg_t *cfg;
    u8 mac[6], bssid[6];
    u32 step, part, part_off;       /* firmware upload cursor */
    wpa2_t wpa;
    u16 channel;                    /* MHz of the AP found by the scan, 0 = unknown */
    u8 found, group_crypt;          /* scan result; WMI group crypto type */
    u8 rxframe[NET_FRAME_MAX];      /* one converted frame waiting for rx() */
    u32 rxlen;
    u8 ep_wmi, ep_data;             /* endpoints the firmware assigned */
    u32 t_state;                    /* now_ms() when the current wait started */
    u32 reconnect_at;               /* now_ms() for the next WMI_CONNECT, 0 = none */
    u8 scan_done, auth_fails;
    u8 byte_mode;                   /* 1: mailbox through CMD52 (the FIFO self-test failed) */
    u8 verbose, switching, from_ios;
    u16 disc_reason, disc_code;
    u32 link_seq;
} A;

static u32 now_ms(void)
{
    u16 hi, lo, hi2;
    do {
        hi = REG_T9_VAL3;
        lo = REG_T9_VAL2;
        hi2 = REG_T9_VAL3;
    } while (hi != hi2);
    return ((u32)hi << 16 | lo) / 65u;      /* 65.457 ticks per ms; 0.7% slow is fine here */
}

static u8 pkt[0x680] __attribute__((aligned(4)));   /* mailbox packet scratch */

/* The network to be on: from wifi.cfg at boot, or from iOS (WLAN_CMD_JOIN).
   have = 0: stay idle (no wifi.cfg, or iOS said "leave"). */
static struct {
    char ssid[33];
    u8 ssid_len, security, have;
    u8 pmk[32];
} T;

/* every BSS seen in scans since the last user scan (docs/wifi.md 10) */
static wlan_bss_t scan_tab[WLAN_SCAN_MAX];
static u32 nscan, scan_seq;
static u8 user_scan;                /* a WLAN_CMD_SCAN is running */

static u32 fail(int err, const char *what)
{
    A.err = err;
    A.state = NET_ST_FAILED;
    log_err("[wifi] %s failed: %d (sdio status %08x)\n", what, err, sdio_last_status());
    return A.state;
}

/* ---- function 1 helpers --------------------------------------------------- */
static int f1_r32(u32 addr, u32 *v)
{
    u32 x = 0;
    for (int i = 0; i < 4; i++) {
        int b = sdio_r8(1, addr + i);
        if (b < 0) return b;
        x |= (u32)b << (8 * i);
    }
    *v = x;
    return 0;
}

static int f1_w32(u32 addr, u32 v)
{
    for (int i = 3; i >= 0; i--) {      /* MSB first: the LSB write triggers */
        int r = sdio_w8(1, addr + i, (u8)(v >> (8 * i)));
        if (r < 0) return r;
    }
    return 0;
}

static int soc_read(u32 addr, u32 *v)
{
    int r = f1_w32(F1_WINDOW_READ_ADDR, addr);
    return r < 0 ? r : f1_r32(F1_WINDOW_DATA, v);
}

static int soc_write(u32 addr, u32 v)
{
    int r = f1_w32(F1_WINDOW_DATA, v);
    return r < 0 ? r : f1_w32(F1_WINDOW_WRITE_ADDR, addr);
}

/* ---- BMI (ROM bootloader) -------------------------------------------------- */
static int bmi_ready(void)
{
    for (int i = 0; i < 20000; i++) {       /* GBATEK: wait for LOCAL_COUNT[4] != 0 */
        int c = sdio_r8(1, F1_COUNT4);
        if (c < 0) return c;
        if (c) return 0;
        waitcycles(335);
    }
    return SDIO_ERR_TIMEOUT;
}

static int bmi_put(const u8 *p, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        int r = sdio_w8(1, F1_MBOX0_BYTE, p[i]);
        if (r < 0) return r;
    }
    return 0;
}

static int bmi_w32(u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    return bmi_put(b, 4);
}

static int bmi_r32(u32 *v)
{
    u32 x = 0;
    for (int i = 0;; i++) {
        int la = sdio_r8(1, F1_RX_LOOKAHEAD_VALID);
        if (la < 0) return la;
        if (la & 1) break;
        if (i > 20000) return SDIO_ERR_TIMEOUT;
        waitcycles(335);
    }
    for (int i = 0; i < 4; i++) {
        int b = sdio_r8(1, F1_MBOX0_BYTE);
        if (b < 0) return b;
        x |= (u32)b << (8 * i);
    }
    *v = x;
    return 0;
}

static int bmi_cmd(u32 cmd, u32 a, u32 b, int nargs)
{
    int r = bmi_ready();
    if (r == 0) r = bmi_w32(cmd);
    if (r == 0 && nargs > 0) r = bmi_w32(a);
    if (r == 0 && nargs > 1) r = bmi_w32(b);
    return r;
}

static int bmi_read_soc(u32 addr, u32 *v)
{
    int r = bmi_cmd(BMI_READ_SOC_REG, addr, 0, 1);
    return r < 0 ? r : bmi_r32(v);
}

static int bmi_write_soc(u32 addr, u32 v) { return bmi_cmd(BMI_WRITE_SOC_REG, addr, v, 2); }

/* one 32-bit BMI_WRITE_MEMORY (host interest words while the ROM runs) */
static int bmi_write_word(u32 addr, u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    int r = bmi_cmd(BMI_WRITE_MEMORY, addr, 4, 2);
    return r < 0 ? r : bmi_put(b, 4);
}

/* one BMI_WRITE_MEMORY / BMI_LZ_DATA chunk (n <= 0x1F0, padded to 4) */
static int bmi_chunk(u32 cmd, u32 addr, const u8 *p, u32 n)
{
    static const u8 zero[4];
    u32 padded = (n + 3) & ~3u;
    int r = cmd == BMI_WRITE_MEMORY ? bmi_cmd(cmd, addr, padded, 2) : bmi_cmd(cmd, padded, 0, 1);
    if (r == 0) r = bmi_put(p, n);
    if (r == 0 && padded != n) r = bmi_put(zero, padded - n);
    return r;
}

/* ---- mailbox packets (HTC framing) ------------------------------------------ */
static int mbox_send(u32 type, u32 ack, u32 ctl, const u8 *body, u32 len)
{
    if (len + 6 > sizeof(pkt)) return SDIO_ERR_ARG;
    u32 total = (len + 6 + SDIO_BLOCK - 1) & ~(SDIO_BLOCK - 1);
    memset(pkt, 0, total);
    pkt[0] = (u8)type; pkt[1] = (u8)ack;
    pkt[2] = (u8)len; pkt[3] = (u8)(len >> 8);
    pkt[4] = (u8)ctl; pkt[5] = (u8)(ctl >> 8);
    if (len) memcpy(pkt + 6, body, len);
    if (A.byte_mode) {                  /* last byte at the window end = end of message */
        for (u32 i = 0; i < total; i++) {
            int r = sdio_w8(1, F1_MBOX0_END - total + i, pkt[i]);
            if (r < 0) return r;
        }
        return 0;
    }
    return sdio_write_blocks(1, F1_MBOX0_END - total, pkt, total);
}

static int wmi_send(u32 id, const void *params, u32 len)
{
    u8 b[72];
    if (len + 2 > sizeof(b)) return SDIO_ERR_ARG;
    b[0] = (u8)id; b[1] = (u8)(id >> 8);
    if (len) memcpy(b + 2, params, len);
    return mbox_send(A.ep_wmi, 1, 0, b, len + 2);   /* flag 1: credit update wanted */
}

/* Read one packet into pkt[]. Returns the payload length (bytes after the
   6-byte header, trailer removed), 0 = nothing pending, < 0 = error. */
static int mbox_recv(u32 *type)
{
    u32 la = 0;
    int st = sdio_r8(1, F1_HOST_INT_STATUS);
    if (st < 0) return st;
    if (!(st & 1)) return 0;
    int r = f1_r32(F1_RX_LOOKAHEAD0, &la);
    if (r < 0) return r;
    u32 len = la >> 16;
    u32 total = (len + 6 + SDIO_BLOCK - 1) & ~(SDIO_BLOCK - 1);
    if (total > sizeof(pkt)) {
        log_warn("[wifi] oversized mailbox packet (lookahead %08x)\n", la);
        return SDIO_ERR_ARG;
    }
    if (A.byte_mode) {
        for (u32 i = 0; i < total; i++) {
            int b = sdio_r8(1, F1_MBOX0_END - total + i);
            if (b < 0) return b;
            pkt[i] = (u8)b;
        }
    } else if ((r = sdio_read_blocks(1, F1_MBOX0_END - total, pkt, total)) < 0) {
        return r;
    }
    *type = pkt[0];
    /* credit / lookahead records at the end (ath6kl HTC_FLG_RX_TRAILER) */
    u32 trailer = (pkt[1] & HTC_FLG_RX_TRAILER) ? pkt[4] : 0;
    return trailer < len ? (int)(len - trailer) : 0;
}

/* ---- firmware upload ---------------------------------------------------------- */
static const fw_part_t *fw_find(u32 kind)
{
    const fw_hdr_t *h = (const fw_hdr_t *)fw_buf;
    for (u32 i = 0; i < h->nparts && i < 8; i++)
        if (h->part[i].kind == kind) return &h->part[i];
    return NULL;
}

static int fw_check(void)
{
    const fw_hdr_t *h = (const fw_hdr_t *)fw_buf;
    if (fw_size < sizeof(*h) || h->magic != FW_MAGIC || h->version != 1 || h->nparts > 8) return -100;
    for (u32 i = 0; i < h->nparts; i++)
        if (h->part[i].off > fw_size || h->part[i].len > fw_size - h->part[i].off) return -101;
    if (!fw_find(FW_STUB_DATA) || !fw_find(FW_STUB_CODE) || !fw_find(FW_DATABASE) || !fw_find(FW_MAIN_LZ))
        return -102;
    return 0;
}

/* Upload order (GBATEK "DSi SD/MMC Firmware Wifi Firmware" + dsiwifi): stub
   data, stub code, execute the stub (it reads the calibration EEPROM), main
   (LZ, decompressed by the ROM), database, then point hi_dset_list_head at it.
   One chunk per call so the monitor loop keeps serving block I/O. */
static int fw_step(void)
{
    static const u8 order[] = { FW_STUB_DATA, FW_STUB_CODE, 0xFF, FW_MAIN_LZ, FW_DATABASE };
    if (A.part >= sizeof(order)) return 1;
    u32 kind = order[A.part];
    int r = 0;
    if (kind == 0xFF) {
        const fw_part_t *c = fw_find(FW_STUB_CODE);
        u32 ret = 0;
        r = bmi_cmd(BMI_EXECUTE, c->dst + 0x400000u, c->dst, 2);    /* code mirror, arg as NWM */
        if (r == 0) r = bmi_r32(&ret);
        log_ok("[wifi] stub executed: r=%d ret=%08x\n", r, ret);
        A.part++;
        return r;
    }
    const fw_part_t *p = fw_find(kind);
    if (A.part_off == 0) {
        logp("[wifi] fw part %u: %u bytes -> %08x\n", kind, p->len, p->dst);
        if (kind == FW_MAIN_LZ && (r = bmi_cmd(BMI_LZ_START, p->dst, 0, 1)) < 0) return r;
    }
    u32 n = p->len - A.part_off;
    if (n > 0x100) n = 0x100;
    if (n) {
        r = bmi_chunk(kind == FW_MAIN_LZ ? BMI_LZ_DATA : BMI_WRITE_MEMORY,
                      p->dst + A.part_off, fw_buf + p->off + A.part_off, n);
        if (r < 0) return r;
        A.part_off += n;
    }
    if (A.part_off >= p->len) {
        if (kind == FW_DATABASE) r = bmi_write_word(HOST_INTEREST + HI_DSET_LIST_HEAD, p->dst);
        A.part++;
        A.part_off = 0;
    }
    return r;
}

/* ---- WMI ------------------------------------------------------------------------ */
static int wmi_connect(void)
{
    int wpa = T.security == WIFI_SEC_WPA2;
    struct __attribute__((packed)) {
        u8 networkType, dot11AuthMode, authMode, pairwiseCryptoType, pairwiseCryptoLen,
           groupCryptoType, groupCryptoLen, ssidLength;
        u8 ssid[32];
        u16 channel;        /* MHz from our scan; 0 + zero BSSID = let the firmware pick */
        u8 bssid[6];
        u32 ctrl_flags;
    } p;
    memset(&p, 0, sizeof(p));
    p.networkType = 1;                          /* infrastructure */
    p.dot11AuthMode = 1;                        /* open system */
    p.authMode = wpa ? AUTH_WPA2_PSK : AUTH_NONE;
    p.pairwiseCryptoType = wpa ? CRYPT_AES : CRYPT_NONE;
    p.groupCryptoType = wpa ? A.group_crypt : CRYPT_NONE;
    p.ssidLength = T.ssid_len;
    memcpy(p.ssid, T.ssid, T.ssid_len);
    if (A.found) {
        p.channel = A.channel;
        memcpy(p.bssid, A.bssid, 6);
    }
    /* dsiwifi's pre-connect settings: max performance power mode, 5 s
       disconnect timeout, no keepalive */
    u8 v = 2;
    wmi_send(WMI_SET_POWER_MODE, &v, 1);
    v = 5;
    wmi_send(WMI_SET_DISC_TIMEOUT, &v, 1);
    v = 0;
    wmi_send(WMI_SET_KEEPALIVE, &v, 1);
    return wmi_send(WMI_CONNECT, &p, sizeof(p));
}

static int wmi_add_key(u32 idx, u32 usage, const u8 *key, u32 keylen, const u8 *rsc)
{
    struct __attribute__((packed)) {
        u8 keyIndex, keyType, keyUsage, keyLength;
        u8 keyRSC[8], key[32];
        u8 key_op_ctrl;
    } p;
    memset(&p, 0, sizeof(p));
    p.keyIndex = (u8)idx; p.keyUsage = (u8)usage; p.keyLength = (u8)keylen;
    p.keyType = keylen == 32 ? CRYPT_TKIP : CRYPT_AES;
    if (rsc) memcpy(p.keyRSC, rsc, 8);
    memcpy(p.key, key, keylen < 16 ? keylen : 16);
    if (keylen == 32) {                 /* TKIP: the firmware wants the MIC keys swapped (dsiwifi) */
        memcpy(p.key + 16, key + 24, 8);
        memcpy(p.key + 24, key + 16, 8);
    }
    p.key_op_ctrl = 3;
    return wmi_send(WMI_ADD_CIPHER_KEY, &p, sizeof(p));
}

static void link_up(void)
{
    u8 one = 1;
    wmi_send(WMI_SYNCHRONIZE, &one, 1);
    A.state = NET_ST_UP;
    A.auth_fails = 0;
    log_ok("[wifi] link up: %s, AP %02x:%02x:%02x:%02x:%02x:%02x\n", T.ssid,
           A.bssid[0], A.bssid[1], A.bssid[2], A.bssid[3], A.bssid[4], A.bssid[5]);
}

static void snonce(u8 out[32])
{
    /* No TRNG on the ARM9: hash the boot ROM's PRNG register, timer and
       everything the chip told us so far. Good enough for a nonce. */
    sha1_ctx c;
    u8 d[20];
    u32 v[8];
    for (int half = 0; half < 2; half++) {
        for (int i = 0; i < 8; i++) v[i] = REG_PRNG ^ (u32)REG_TIMER0 << 16 ^ sdio_last_status();
        sha1_init(&c);
        sha1_update(&c, (const u8 *)v, sizeof(v));
        sha1_update(&c, A.mac, 6);
        sha1_update(&c, A.bssid, 6);
        sha1_update(&c, out, 16);
        sha1_final(&c, d);
        memcpy(out + 16 * half, d, 16);
    }
}

static void on_eapol(const u8 *eth, u32 len)
{
    u8 frame[14 + 160], out[2 + 14 + 8 + 160];
    u32 rlen = 0;
    int ev = wpa2_rx_eapol(&A.wpa, eth + 14, len - 14, frame + 14, sizeof(frame) - 14, &rlen);
    if (ev < 0) {
        log_warn("[wifi] EAPOL-Key rejected: %d\n", ev);
        return;
    }
    if (ev == WPA2_EV_NONE) return;
    memcpy(frame, A.bssid, 6);
    memcpy(frame + 6, A.mac, 6);
    frame[12] = 0x88; frame[13] = 0x8E;
    u32 n = wifi_frame_to_wmi(frame, 14 + rlen, out, sizeof(out));
    int r = n ? mbox_send(A.ep_data, 0, 0, out, n) : SDIO_ERR_ARG;
    logp("[wifi] EAPOL event %d, reply %u bytes: %d\n", ev, rlen, r);
    if (ev == WPA2_EV_KEYS || ev == WPA2_EV_GTK) {
        if (ev == WPA2_EV_KEYS) wmi_add_key(0, 0, A.wpa.tk, 16, NULL);          /* pairwise */
        wmi_add_key(A.wpa.gtk_idx, 1, A.wpa.gtk, A.wpa.gtk_len, A.wpa.rsc);     /* group */
        if (A.state != NET_ST_UP) link_up();
    }
}

/* SET_BSS_FILTER(1 = all BSSs) + a specific probed SSID (hidden networks) +
   START_SCAN on all channels; parameter layouts as in dsiwifi */
static int scan_start(void)
{
    struct __attribute__((packed)) { u8 filter, a1; u16 a2; u32 ieMask; } f = { 1, 0, 0, 0 };
    struct __attribute__((packed)) { u8 index, flag, len; u8 ssid[32]; } ps;
    struct __attribute__((packed)) {
        u32 forceFgScan, isLegacy, homeDwellTime, forceScanInterval;
        u8 scanType, numChannels; u16 channelList;
    } sc = { 0, 0, 20, 0, 0, 0, 0 };
    memset(&ps, 0, sizeof(ps));
    if (T.have) {                               /* probe for it too (hidden networks) */
        ps.flag = 1;                            /* SPECIFIC_SSID_FLAG */
        ps.len = T.ssid_len;
        memcpy(ps.ssid, T.ssid, T.ssid_len);
    }
    int r = wmi_send(WMI_SET_BSS_FILTER, &f, sizeof(f));
    if (r >= 0) r = wmi_send(WMI_SET_PROBED_SSID, &ps, sizeof(ps));
    if (r >= 0) r = wmi_send(WMI_START_SCAN, &sc, sizeof(sc));
    A.scan_done = 0;
    A.t_state = now_ms();
    return r;
}

/* WMI_BSSINFO_EVENT: u16 channel, u8 frametype, u8 snr, s16 rssi, u8 bssid[6],
   u32 ieMask, then the beacon/probe-response body (8 timestamp, 2 interval,
   2 capability, IEs). Remember our SSID's channel, BSSID and group cipher. */
static void scan_record(const u8 *d, u32 n)
{
    const u8 *ie = d + 28, *end = d + n;
    wlan_bss_t *b = NULL;
    u32 i;
    for (i = 0; i < nscan; i++)
        if (!memcmp(scan_tab[i].bssid, d + 6, 6)) b = &scan_tab[i];
    if (!b) {
        if (nscan >= WLAN_SCAN_MAX) return;
        b = &scan_tab[nscan++];
    }
    memset(b, 0, sizeof(*b));
    memcpy(b->bssid, d + 6, 6);
    b->channel_mhz = (u16)(d[0] | d[1] << 8);
    b->rssi = (int16_t)(d[4] | d[5] << 8);
    b->noise = (int16_t)(b->rssi - (int8_t)d[3]);
    b->beacon_int = (u16)(d[24] | d[25] << 8);
    b->caps = (u16)(d[26] | d[27] << 8);
    if (b->caps & 0x10) b->flags |= WLAN_BSS_PRIVACY;
    for (const u8 *p = ie; p + 2 <= end && p + 2 + p[1] <= end; p += 2 + p[1]) {
        if (p[0] == 0 && p[1] <= 32 && !b->ssid_len) {
            b->ssid_len = p[1];
            memcpy(b->ssid, p + 2, p[1]);
        }
        if (p[0] == 0x30) b->flags |= WLAN_BSS_RSN;
        if (p[0] == 0xDD && p[1] >= 4 && p[2] == 0x00 && p[3] == 0x50 && p[4] == 0xF2 && p[5] == 1)
            b->flags |= WLAN_BSS_WPA;
        if ((u32)b->ie_len + 2 + p[1] <= WLAN_IE_MAX) {
            memcpy(b->ie + b->ie_len, p, 2 + p[1]);
            b->ie_len = (u8)(b->ie_len + 2 + p[1]);
        }
    }
}

static void on_bssinfo(const u8 *d, u32 n)
{
    if (n < 28) return;
    scan_record(d, n);
    if (!T.have) return;
    const u8 *ie = d + 28, *end = d + n;
    int match = 0, group = CRYPT_AES;
    while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        if (ie[0] == 0 && ie[1] == T.ssid_len && !memcmp(ie + 2, T.ssid, ie[1])) match = 1;
        if (ie[0] == 0x30 && ie[1] >= 6) group = ie[7] == 2 ? CRYPT_TKIP : CRYPT_AES;  /* RSN group suite 00-0F-AC-xx */
        ie += 2 + ie[1];
    }
    if (!match || (A.found && d[3] <= A.found)) return;   /* keep the strongest (by SNR) */
    A.found = d[3] ? d[3] : 1;
    A.channel = (u16)(d[0] | d[1] << 8);
    memcpy(A.bssid, d + 6, 6);
    A.group_crypt = (u8)group;
}

static void do_connect(void)
{
    int r = wmi_connect();
    logp("[wifi] connect \"%s\" (%s%s), %s: %d\n", T.ssid,
         T.security == WIFI_SEC_WPA2 ? "WPA2-PSK/CCMP" : "open",
         A.group_crypt == CRYPT_TKIP ? ", TKIP group" : "",
         A.found ? "seen in scan" : "not seen in scan, firmware picks", r);
    A.state = r < 0 ? NET_ST_FAILED : NET_ST_SCAN;
    A.err = r;
    A.scan_done = 1;                            /* scan over: no scan timeout */
    A.reconnect_at = 0;
    A.t_state = now_ms();
}

static void on_wmi(const u8 *b, u32 len)
{
    if (len < 2) return;
    u32 id = b[0] | (u32)b[1] << 8;
    const u8 *d = b + 2;
    u32 n = len - 2;
    switch (id) {
    case WMI_EV_READY: {
        if (n >= 6) memcpy(A.mac, d, 6);
        log_ok("[wifi] WMI ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
               A.mac[0], A.mac[1], A.mac[2], A.mac[3], A.mac[4], A.mac[5]);
        u32 v = 0x7F;
        wmi_send(WMI_TARGET_ERROR_BITMASK, &v, 4);
        v = 0;                                  /* GBATEK: timeout 0 = no heartbeat (Type4 firmware needs this) */
        wmi_send(WMI_SET_HEARTBEAT_TIMEOUT, &v, 4);
        A.group_crypt = CRYPT_AES;
        if (!T.have) {
            logp("[wifi] idle: no network configured, waiting for iOS (Settings > Wi-Fi)\n");
            A.state = NET_ST_IDLE;
            break;
        }
        int r = scan_start();
        logp("[wifi] scanning for \"%s\": %d\n", T.ssid, r);
        A.state = r < 0 ? NET_ST_FAILED : NET_ST_SCAN;
        A.err = r;
        break;
    }
    case WMI_EV_BSSINFO:
        on_bssinfo(d, n);
        break;
    case WMI_EV_SCAN_COMPLETE:
        if (user_scan) {
            user_scan = 0;
            scan_seq++;
            logp("[wifi] scan done: %u networks\n", nscan);
        }
        if (A.state == NET_ST_SCAN && !A.scan_done) do_connect();
        break;
    case WMI_EV_CONNECT:
        /* u16 channel; u8 bssid[6]; ... (ath6kl struct wmi_connect_event) */
        if (n >= 8) memcpy(A.bssid, d + 2, 6);
        logp("[wifi] associated, channel %u MHz\n", n >= 2 ? d[0] | (u32)d[1] << 8 : 0);
        if (T.security == WIFI_SEC_WPA2) {
            u8 sn[32];
            memset(sn, 0, sizeof(sn));
            snonce(sn);
            wpa2_start(&A.wpa, T.pmk, A.mac, A.bssid, sn);
            A.wpa.group_cipher = A.group_crypt == CRYPT_TKIP ? 2 : 4;   /* must match our association IE */
            A.state = NET_ST_AUTH;
        } else {
            link_up();
        }
        break;
    case WMI_EV_DISCONNECT:
        /* u16 802.11 reason/status, u8 bssid[6], u8 WMI reason (ath6kl) */
        A.disc_reason = n >= 9 ? d[8] : 0;
        A.disc_code = n >= 2 ? (u16)(d[0] | d[1] << 8) : 0;
        A.link_seq++;
        if (A.state == NET_ST_AUTH && ++A.auth_fails == 3)
            log_err("[wifi] the WPA2 handshake failed 3 times: wrong password?\n");
        if (!T.have) {                          /* iOS asked to leave (or never joined) */
            logp("[wifi] disconnected (WMI reason %u): idle\n", A.disc_reason);
            A.state = NET_ST_IDLE;
            break;
        }
        if (A.switching) {                      /* leaving the old network for a new one */
            A.switching = 0;
            A.state = NET_ST_SCAN;
            A.scan_done = 1;
            A.reconnect_at = 1;
            break;
        }
        if (A.auth_fails >= 3 && A.from_ios) {  /* a wrong password typed in Settings: give up */
            log_warn("[wifi] disconnected (WMI reason %u, 802.11 code %u): giving up on \"%s\"\n",
                     A.disc_reason, A.disc_code, T.ssid);
            T.have = 0;
            A.state = NET_ST_IDLE;
            break;
        }
        log_warn("[wifi] disconnected (WMI reason %u, 802.11 code %u), reconnecting in 5 s\n",
                 A.disc_reason, A.disc_code);
        A.state = NET_ST_SCAN;
        A.scan_done = 1;
        A.reconnect_at = now_ms() + 5000;
        if (!A.reconnect_at) A.reconnect_at = 1;
        break;
    default:
        if (A.verbose) logp("[wifi] WMI event %04x, %u bytes\n", id, n);
        break;
    }
}

static void on_htc(const u8 *b, u32 len)
{
    if (len < 2) return;
    u32 id = b[0] | (u32)b[1] << 8;
    if (id != HTC_READY) return;
    /* connect the control + 4 data services, then SETUP_COMPLETE (dsiwifi htc.twl.c) */
    static const u8 svc[5][8] = {
        { HTC_CONN_SVC, 0, 0, 1, 0, 0, 0, 0 }, { HTC_CONN_SVC, 0, 1, 1, 5, 0, 0, 0 },
        { HTC_CONN_SVC, 0, 2, 1, 5, 0, 0, 0 }, { HTC_CONN_SVC, 0, 3, 1, 5, 0, 0, 0 },
        { HTC_CONN_SVC, 0, 4, 1, 5, 0, 0, 0 },
    };
    static const u8 done[2] = { HTC_SETUP_COMPLETE, 0 };
    u8 eid[5] = { 0 };
    int r = 0;
    logp("[wifi] HTC ready: %u credits of %u bytes\n", len >= 6 ? b[2] | b[3] << 8 : 0,
         len >= 6 ? b[4] | b[5] << 8 : 0);
    for (int i = 0; i < 5 && r >= 0; i++) {
        u32 type;
        r = mbox_send(PKT_HTC, 0, 0, svc[i], 8);
        int t;
        for (t = 0; r >= 0 && t < 2000; t++) {          /* its response */
            int got = mbox_recv(&type);
            if (got < 0) { r = got; break; }
            /* CONNECT_SERVICE_RESP: id 3, service, status, endpoint, max size */
            const u8 *q = pkt + 6;
            if (got >= 6 && type == PKT_HTC && q[0] == 3 && q[2] == svc[i][2] && q[3] == svc[i][3]) {
                if (q[4] != 0 || q[5] == 0 || q[5] > 7) r = -120 - q[4];
                eid[i] = q[5];
                break;
            }
            if (got == 0) waitcycles(3350);             /* nothing, or a credit report */
        }
        if (r >= 0 && t >= 2000) r = SDIO_ERR_TIMEOUT;
    }
    A.ep_wmi = eid[0];
    A.ep_data = eid[1];
    if (r >= 0) r = mbox_send(PKT_HTC, 0, 0, done, 2);
    if (r >= 0) r = f1_w32(F1_INT_STATUS_EN, 0x010300D1);
    logp("[wifi] HTC services connected: %d (WMI endpoint %u, data %u/%u/%u/%u)\n", r,
         eid[0], eid[1], eid[2], eid[3], eid[4]);
    A.state = r < 0 ? NET_ST_FAILED : NET_ST_WMI;
    A.t_state = now_ms();
    A.err = r;
}

/* Drain one mailbox packet. Data frames end up in A.rxframe. */
static void pump(void)
{
    u32 type = 0;
    int n = mbox_recv(&type);
    if (n <= 0) {
        if (n < 0 && A.verbose) log_warn("[wifi] mailbox read: %d\n", n);
        return;
    }
    const u8 *body = pkt + 6;
    if (type == PKT_HTC) {
        on_htc(body, (u32)n);
    } else if (type == A.ep_wmi) {
        on_wmi(body, (u32)n);
    } else if (A.ep_wmi) {                              /* the data endpoints */
        static u8 eth[NET_FRAME_MAX];
        u16 et = 0;
        u32 len = wifi_frame_from_wmi(body, (u32)n, eth, sizeof(eth), &et);
        if (!len) return;
        if (et == 0x888E) {
            if (T.security == WIFI_SEC_WPA2 && A.state == NET_ST_AUTH) on_eapol(eth, len);
        } else if (A.state == NET_ST_UP && !A.rxlen) {
            memcpy(A.rxframe, eth, len);
            A.rxlen = len;
        }
    }
}

/* ---- iOS control plane (WLAN_CMD_*, docs/wifi.md section 10) ----------------------- */
static int fw_up(void)
{
    return A.state == NET_ST_IDLE || A.state == NET_ST_SCAN || A.state == NET_ST_AUTH ||
           A.state == NET_ST_UP;
}

/* associated or a WMI_CONNECT outstanding (the firmware will report back) */
static int connecting(void)
{
    return A.state == NET_ST_AUTH || A.state == NET_ST_UP ||
           (A.state == NET_ST_SCAN && A.scan_done && !A.reconnect_at);
}

static int user_scan_start(void)
{
    struct __attribute__((packed)) { u8 filter, a1; u16 a2; u32 ieMask; } f = { 1, 0, 0, 0 };
    struct __attribute__((packed)) {
        u32 forceFgScan, isLegacy, homeDwellTime, forceScanInterval;
        u8 scanType, numChannels; u16 channelList;
    } sc = { 0, 0, 20, 0, 0, 0, 0 };
    nscan = 0;
    user_scan = 1;
    int r = wmi_send(WMI_SET_BSS_FILTER, &f, sizeof(f));
    if (r >= 0) r = wmi_send(WMI_START_SCAN, &sc, sizeof(sc));
    return r;
}

/* choose channel, BSSID and group cipher for T from the scan table (strongest) */
static void target_lookup(void)
{
    const wlan_bss_t *best = NULL;
    for (u32 i = 0; i < nscan; i++) {
        const wlan_bss_t *b = &scan_tab[i];
        if (b->ssid_len == T.ssid_len && !memcmp(b->ssid, T.ssid, T.ssid_len) &&
            (!best || b->rssi > best->rssi))
            best = b;
    }
    A.found = 0;
    A.group_crypt = CRYPT_AES;
    if (!best) return;
    A.found = 1;
    A.channel = best->channel_mhz;
    memcpy(A.bssid, best->bssid, 6);
    for (u32 i = 0; i + 2 <= best->ie_len; i += 2 + best->ie[i + 1])
        if (best->ie[i] == 0x30 && best->ie[i + 1] >= 6)
            A.group_crypt = best->ie[i + 7] == 2 ? CRYPT_TKIP : CRYPT_AES;
}

int ar6k_command(const volatile wlan_cmd_t *c)
{
    if (!fw_up()) return -1;
    u32 zero = 0;
    switch (c->cmd) {
    case WLAN_CMD_SCAN:
        logp("[wifi] iOS: scan\n");
        return user_scan_start();
    case WLAN_CMD_JOIN: {
        if (!c->ssid_len || c->ssid_len > 32) return -2;
        int was = connecting();
        memset(&T, 0, sizeof(T));
        for (u32 i = 0; i < c->ssid_len; i++) T.ssid[i] = c->ssid[i];
        T.ssid_len = c->ssid_len;
        T.security = c->security == WLAN_SEC_WPA2 ? WIFI_SEC_WPA2 : WIFI_SEC_OPEN;
        for (u32 i = 0; i < 32; i++) T.pmk[i] = c->pmk[i];
        T.have = 1;
        A.from_ios = 1;
        A.auth_fails = 0;
        target_lookup();
        logp("[wifi] iOS: join \"%s\" (%s), %s\n", T.ssid, T.security == WIFI_SEC_WPA2 ? "WPA2" : "open",
             A.found ? "in the scan list" : "not in the scan list: scanning");
        if (was) {                              /* leave the current network first */
            A.switching = 1;
            return wmi_send(WMI_DISCONNECT, &zero, 4);
        }
        if (A.found) {
            do_connect();
            return A.err;
        }
        A.state = NET_ST_SCAN;
        return scan_start();
    }
    case WLAN_CMD_LEAVE:
        logp("[wifi] iOS: leave \"%s\"\n", T.ssid);
        T.have = 0;
        A.reconnect_at = 0;
        if (connecting()) return wmi_send(WMI_DISCONNECT, &zero, 4);
        A.state = NET_ST_IDLE;
        return 0;
    }
    return -3;
}

/* status + scan table into the wlan mailbox (wifi.c, every poll) */
void ar6k_publish(volatile wlanmbox_t *w, u32 *published_scan)
{
    volatile wlan_status_t *st = &w->st;
    u32 ws;
    switch (A.state) {
    case NET_ST_POWER: case NET_ST_BMI: case NET_ST_FWLOAD: case NET_ST_BOOT:
    case NET_ST_HTC: case NET_ST_WMI: ws = WLAN_ST_STARTING; break;
    case NET_ST_IDLE: ws = WLAN_ST_IDLE; break;
    case NET_ST_SCAN: ws = T.have ? WLAN_ST_JOINING : WLAN_ST_IDLE; break;
    case NET_ST_AUTH: ws = WLAN_ST_JOINING; break;
    case NET_ST_UP: ws = WLAN_ST_UP; break;
    default: ws = WLAN_ST_OFF; break;
    }
    st->state = ws;
    for (int i = 0; i < 6; i++) st->mac[i] = A.mac[i];
    st->link_seq = A.link_seq;
    st->disc_reason = A.disc_reason;
    st->disc_code = A.disc_code;
    if (ws == WLAN_ST_UP || ws == WLAN_ST_JOINING) {
        int16_t rssi = -60, noise = -95;
        for (u32 i = 0; i < nscan; i++)
            if (!memcmp(scan_tab[i].bssid, A.bssid, 6)) { rssi = scan_tab[i].rssi; noise = scan_tab[i].noise; }
        for (int i = 0; i < 6; i++) st->bssid[i] = A.bssid[i];
        for (int i = 0; i < 32; i++) st->ssid[i] = i < T.ssid_len ? T.ssid[i] : 0;
        st->ssid_len = T.ssid_len;
        st->security = T.security == WIFI_SEC_WPA2 ? WLAN_SEC_WPA2 : WLAN_SEC_OPEN;
        st->channel_mhz = A.channel;
        st->rssi = rssi;
        st->noise = noise;
    } else {
        st->ssid_len = 0;
    }
    if (*published_scan != scan_seq) {
        *published_scan = scan_seq;
        volatile u8 *d = (volatile u8 *)w->bss;
        const u8 *src = (const u8 *)scan_tab;
        for (u32 i = 0; i < nscan * sizeof(wlan_bss_t); i++) d[i] = src[i];
        st->nscan = nscan;
        st->scan_seq = scan_seq;
    }
}

/* ---- backend interface ------------------------------------------------------------ */
static int ar6k_init(const wifi_cfg_t *cfg, u8 mac[6])
{
    memset(&A, 0, sizeof(A));
    A.cfg = cfg;
    A.verbose = (u8)cfg->verbose;
    memset(&T, 0, sizeof(T));
    if (cfg->ssid_len) {                    /* wifi.cfg preset: join it at boot */
        memcpy(T.ssid, cfg->ssid, cfg->ssid_len);
        T.ssid_len = cfg->ssid_len;
        T.security = cfg->security;
        memcpy(T.pmk, cfg->pmk, 32);
        T.have = 1;
    }
    memset(mac, 0, 6);          /* known after WMI ready; wifi.c copies it then */
    int r = sd_load(cfg->fw, fw_buf, sizeof(fw_buf), &fw_size);
    if (r < 0) {
        if (cfg->ssid_len)
            log_warn("[wifi] %s: not found (%d). Create it with tools/ios3ds-wifi/nwmfw.py\n", cfg->fw, r);
        else
            logp("[wifi] %s: not found (%d): Wi-Fi off (docs/wifi.md section 3)\n", cfg->fw, r);
        return -1;
    }
    if ((r = fw_check()) < 0) {
        log_err("[wifi] %s: bad container (%d)\n", cfg->fw, r);
        return -1;
    }
    A.state = NET_ST_POWER;
    return 0;
}

/* Unverified on hardware: whether the ARM9 can use TMIO controller 2's 32-bit
   FIFO at 0x1012210C (dsiwifi's 3DS ARM11 build uses a FIFO mirror at
   +0x200000 instead). Write a pattern into the function-1 scratch registers
   with CMD52, read it back with one CMD53; if that fails, move every mailbox
   transfer to CMD52 (slow, but needs no FIFO). */
static void fifo_selftest(void)
{
    static const u8 pat[4] = { 0x5A, 0xC3, 0x0F, 0x96 };
    u8 got[4] __attribute__((aligned(4))) = { 0 };
    int r = 0;
    for (int i = 0; i < 4 && r >= 0; i++) r = sdio_w8(1, F1_SCRATCH0 + i, pat[i]);
    if (r >= 0) r = sdio_read_bytes(1, F1_SCRATCH0, got, 4);
    A.byte_mode = r < 0 || memcmp(got, pat, 4) != 0;
    if (A.byte_mode)
        log_warn("[wifi] TMIO2 32-bit FIFO check failed (%d, read %02x%02x%02x%02x, status %08x): "
                 "mailbox falls back to CMD52 byte transfers\n", r, got[0], got[1], got[2], got[3],
                 sdio_last_status());
    else
        logp("[wifi] TMIO2 32-bit FIFO works (CMD53 read back the scratch pattern)\n");
}

static u32 ar6k_poll(void)
{
    u32 v = 0;
    int r;
    switch (A.state) {
    case NET_ST_POWER:
        /* libn3ds WIFI_init: CFG11 "wifi power" (really the MP regs), select
           the SDIO chip, release nRESET, then ~2 ms ("very close to 2 ms")
           before talking SDIO. We pulse reset first because Horizon may
           have left the chip running (warm reboot through Luma). */
        CFG11_WIFICNT = 1;
        GPIO_WIFI_MODE = (u16)(GPIO_WIFI_MODE & ~1u);
        GPIO_WIFI_EN = (u16)(GPIO_WIFI_EN & ~1u);
        waitcycles(33500);                              /* ~1 ms in reset */
        GPIO_WIFI_EN = (u16)(GPIO_WIFI_EN | 1u);
        waitcycles(67000);                              /* ~2 ms */
        if ((r = sdio_init()) < 0) return fail(r, "SDIO enumeration");
        if ((r = soc_read(SOC_CHIP_ID, &v)) < 0) return fail(r, "chip id read");
        log_ok("[wifi] SDIO up, chip id %08x%s\n", v, v == CHIP_ID_AR6014 ? " (AR6014)" : " (unexpected)");
        fifo_selftest();
        if ((r = soc_write(SOC_RESET_CONTROL, 0x100)) < 0) return fail(r, "warm reset");
        waitcycles(3350000);
        A.state = NET_ST_BMI;
        break;
    case NET_ST_BMI:
        if ((r = bmi_cmd(BMI_GET_TARGET_ID, 0, 0, 0)) < 0 || (r = bmi_r32(&v)) < 0)
            return fail(r, "BMI_GET_TARGET_ID");
        if (v == 0xFFFFFFFFu) {                         /* extended reply: size, version, type */
            u32 sz = 0, ver = 0, x;
            if ((r = bmi_r32(&sz)) < 0 || (r = bmi_r32(&ver)) < 0) return fail(r, "BMI target info");
            for (u32 i = 8; i < sz && i < 32; i += 4) bmi_r32(&x);
            v = ver;
        }
        log_ok("[wifi] BMI ROM version %08x (AR6014 ROM is 2300006F)\n", v);
        /* dsiwifi: hi_app_host_interest = 2, then scratch0 |= 8,
           WLAN_SYSTEM_SLEEP |= 1, CLOCK_CONTROL = 5, [0x4020] = 0 */
        if ((r = bmi_write_word(HOST_INTEREST, 2)) < 0) return fail(r, "host interest");
        if ((r = bmi_read_soc(SOC_SCRATCH0, &v)) < 0 || (r = bmi_write_soc(SOC_SCRATCH0, v | 8)) < 0 ||
            (r = bmi_read_soc(SOC_WLAN_SYS_SLEEP, &v)) < 0 || (r = bmi_write_soc(SOC_WLAN_SYS_SLEEP, v | 1)) < 0 ||
            (r = bmi_write_soc(SOC_CLOCK_CONTROL, 5)) < 0 || (r = bmi_write_soc(SOC_REG_4020, 0)) < 0)
            return fail(r, "BMI clock setup");
        A.state = NET_ST_FWLOAD;
        break;
    case NET_ST_FWLOAD:
        r = fw_step();
        if (r < 0) return fail(r, "firmware upload");
        if (r == 1) {
            if ((r = bmi_write_word(HOST_INTEREST + HI_MBOX_IO_BLOCK_SZ, SDIO_BLOCK)) < 0 ||
                (r = bmi_write_word(HOST_INTEREST + HI_MBOX_ISR_YIELD, 0x63)) < 0 ||
                (r = bmi_cmd(BMI_DONE, 0, 0, 0)) < 0)
                return fail(r, "BMI_DONE");
            logp("[wifi] firmware started, waiting for it to come up\n");
            A.t_state = now_ms();
            A.state = NET_ST_BOOT;
        }
        break;
    case NET_ST_BOOT:
        if ((r = soc_read(HOST_INTEREST + HI_BOARD_DATA_INIT, &v)) < 0) return fail(r, "boot poll");
        if (v == 1) {
            log_ok("[wifi] firmware running (board data initialised)\n");
            A.state = NET_ST_HTC;
            A.t_state = now_ms();
        } else if (now_ms() - A.t_state > 3000) {
            return fail(-110, "firmware start (hi_board_data_initialized never set)");
        }
        break;
    case NET_ST_SCAN:
        /* no SCAN_COMPLETE within 10 s: connect with what we have and let
           the firmware search; after a disconnect: reconnect when due */
        if (!A.scan_done && now_ms() - A.t_state > 10000) do_connect();
        if (A.reconnect_at && (int)(now_ms() - A.reconnect_at) >= 0) do_connect();
        if (!A.rxlen) pump();
        break;
    case NET_ST_HTC: case NET_ST_WMI:
        /* the firmware announces itself (HTC_READY) and WMI (WMI_READY)
           within milliseconds; give up after 5 s instead of waiting forever */
        if (now_ms() - A.t_state > 5000)
            return fail(-111, A.state == NET_ST_HTC ? "firmware start (no HTC_READY)" : "WMI start (no WMI_READY)");
        if (!A.rxlen) pump();
        break;
    case NET_ST_AUTH: case NET_ST_UP: case NET_ST_IDLE:
        if (!A.rxlen) pump();       /* keep a data frame until rx() took it */
        break;
    default:
        break;
    }
    return A.state;
}

static int ar6k_tx(const u8 *eth, u32 len)
{
    static u8 out[2 + 14 + 8 + 1500];
    if (A.state != NET_ST_UP) return -1;
    u32 n = wifi_frame_to_wmi(eth, len, out, sizeof(out));
    return n ? mbox_send(A.ep_data, 0, 0, out, n) : -2;
}

static u32 ar6k_rx(u8 *eth, u32 cap)
{
    if (!A.rxlen) pump();
    u32 n = A.rxlen;
    if (!n || n > cap) { A.rxlen = 0; return 0; }
    memcpy(eth, A.rxframe, n);
    A.rxlen = 0;
    return n;
}

static int ar6k_last_error(void) { return A.err; }

/* wifi.c reads the MAC through this once the firmware reported it */
const u8 *ar6k_mac(void) { return A.mac; }

const wifi_backend_t wifi_backend_ar6014 = {
    .name = "ar6014", .id = NET_BACKEND_AR6014,
    .init = ar6k_init, .poll = ar6k_poll, .tx = ar6k_tx, .rx = ar6k_rx,
    .last_error = ar6k_last_error,
};
