/*
 * ios3ds QEMU board: the Old 3DS Wi-Fi slot, modelled at the bus level so that
 * the ARM9 driver (boot/arm9/wifi/sdio.c, ar6k.c, wpa2.c) is tested against
 * the protocol, not against a shortcut.
 *
 *   ARM9/ARM11 --MMIO--> TMIO controller 2 (0x10122000, polled, no IRQ)
 *     --SDIO CMD5/3/7/52/53--> AR6014-like card: CCCR/FBR/CIS, function 1
 *       registers (HOST_INT_STATUS, lookahead, COUNT_DEC credits, the
 *       diagnostic window), mailbox 0
 *     --BMI (ROM)--> firmware upload bookkeeping (contents are NOT run)
 *     --HTC/WMI (firmware)--> services, scan, connect, keys, data endpoints
 *     --"air"--> an access point with its own WPA2-PSK authenticator
 *       (4-way handshake written from IEEE 802.11i, independent of wpa2.c)
 *     --Ethernet II--> a QEMU NIC backend (-nic user,model=ctr-wifi: slirp
 *                DHCP, DNS and NAT to the host)
 *
 * Power: CFG11_WIFICNT (0x10140180) bit 0 and GPIO 3_12 (nRESET, 0x10147028
 * bit 0) must be 1 and GPIO 2_2 (0x10147014 bit 0, 0 = Atheros SDIO) must be
 * 0, as libn3ds WIFI_init does; nRESET low resets the card completely.
 *
 * Sources for every protocol detail, and the places where the model has to
 * guess, are listed in docs/wifi.md section 7: Linux ath6kl (target.h, htc.h,
 * wmi.h, bmi.c, sdio.c), dsiwifi (MIT; runs on real DSi/3DS hardware), GBATEK
 * "DSi Atheros Wifi", the SDIO simplified spec and IEEE 802.11i-2004. The
 * firmware itself is not executed: BMI uploads are checked for plausibility
 * (sizes, addresses, order) and the "firmware" is this file's HTC/WMI code.
 *
 * Properties (-global ctr-wifi.<name>=<value>): ssid, psk, security
 * (wpa2|open), group (ccmp|tkip), channel (1-13), hidden, fifo32, decoys, debug (0-3), mac.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "net/net.h"
#include "crypto/aes.h"
#include "crypto/hash.h"
#include "ctr_wifi.h"

#define TYPE_CTR_WIFI "ctr-wifi"
typedef struct CtrWifiState CtrWifiState;
#define CTR_WIFI(obj) OBJECT_CHECK(CtrWifiState, (obj), TYPE_CTR_WIFI)

/* ---- TMIO register subset (same block as controller 1) ------------------ */
#define T_CMD        0x00
#define T_PORTSEL    0x02
#define T_ARG0       0x04
#define T_ARG1       0x06
#define T_BLKCNT     0x0A
#define T_RESP0      0x0C
#define T_STAT0      0x1C
#define T_STAT1      0x1E
#define T_CLKCTL     0x24
#define T_BLKLEN     0x26
#define T_OPT        0x28
#define T_ERRSTAT0   0x2C
#define T_FIFO16     0x30
#define T_RESET      0xE0
#define T_DATA32CTL  0x100
#define T_BLKLEN32   0x104
#define T_FIFO32     0x10C

#define ST0_RESPEND   0x0001
#define ST0_DATAEND   0x0004
#define ST1_CRCFAIL   0x0002
#define ST1_DATATMO   0x0008
#define ST1_CMDTMO    0x0040
#define ST1_NOTBUSY   0x0080
#define ST1_RXRDY     0x0100
#define ST1_TXRQ      0x0200
#define ST1_CLKDIVOK  0x2000
#define D32_EN        0x0002
#define D32_FULL      0x0100
#define D32_RW        0x1802

/* ---- AR6014 facts (docs/wifi.md sections 1 and 7) ----------------------- */
#define AR_CHIP_ID          0x0D000001u
#define AR_ROM_VERSION      0x2300006Fu
#define AR_RAM_BASE         0x00500000u
#define AR_RAM_SIZE         0x00040000u     /* 256 KiB modelled */
#define AR_IRAM_MIRROR      0x00400000u     /* 0x9xxxxx = code view of 0x5xxxxx */
#define AR_HI               0x00520000u     /* host interest area */
#define HI_DSET_LIST_HEAD   0x18
#define HI_BOARD_DATA       0x54
#define HI_BOARD_DATA_INIT  0x58
#define HI_MBOX_IO_BLOCK_SZ 0x6C
#define SOC_RESET_CONTROL   0x00004000u
#define SOC_RESET_CAUSE     0x000040C0u
#define SOC_CHIP_ID         0x000040ECu

enum { BMI_NOP = 0, BMI_DONE, BMI_READ_MEMORY, BMI_WRITE_MEMORY, BMI_EXECUTE,
       BMI_SET_APP_START, BMI_READ_SOC_REG, BMI_WRITE_SOC_REG, BMI_GET_TARGET_ID,
       BMI_LZ_START = 0xD, BMI_LZ_DATA = 0xE };
#define BMI_DATA_MAX 0x200                  /* dsiwifi sends 0x1F8-byte chunks */

/* function 1 registers (ath6kl target.h) */
#define F1_HOST_INT_STATUS  0x400
#define F1_CPU_INT_STATUS   0x401
#define F1_ERROR_INT_STATUS 0x402
#define F1_COUNTER_INT_STATUS 0x403
#define F1_MBOX_FRAME       0x404
#define F1_RX_LOOKAHEAD_VALID 0x405
#define F1_RX_LOOKAHEAD0    0x408
#define F1_COUNT            0x420
#define F1_COUNT_DEC        0x440
#define F1_WINDOW_DATA      0x474
#define F1_WINDOW_WRITE     0x478
#define F1_WINDOW_READ      0x47C
#define ERR_TX_OVERFLOW     0x01
#define ERR_RX_UNDERFLOW    0x02

/* HTC (ath6kl htc.h) */
#define HTC_FLG_NEED_CREDIT 0x01
#define HTC_FLG_RX_TRAILER  0x02
#define HTC_REC_CREDITS     1
#define HTC_CREDITS         8
#define HTC_CREDIT_SIZE     1664

/* WMI ids of the AR6002-era firmware the DSi/3DS use (2-byte command header,
   enumerated auth/crypto values; GBATEK, dsiwifi ath/wmi.h) */
enum {
    WMI_CONNECT = 0x0001, WMI_RECONNECT = 0x0002, WMI_DISCONNECT = 0x0003,
    WMI_SYNCHRONIZE = 0x0004, WMI_CREATE_PSTREAM = 0x0005, WMI_START_SCAN = 0x0007,
    WMI_SET_SCAN_PARAMS = 0x0008, WMI_SET_BSS_FILTER = 0x0009, WMI_SET_PROBED_SSID = 0x000A,
    WMI_SET_LISTEN_INT = 0x000B, WMI_SET_DISC_TIMEOUT = 0x000D, WMI_GET_CHANNEL_LIST = 0x000E,
    WMI_SET_CHANNEL_PARAMS = 0x0011, WMI_SET_POWER_MODE = 0x0012, WMI_ADD_CIPHER_KEY = 0x0016,
    WMI_DELETE_CIPHER_KEY = 0x0017, WMI_SET_TX_PWR = 0x001B, WMI_DELETE_BAD_AP = 0x001F,
    WMI_TARGET_ERROR_BITMASK = 0x0022, WMI_WMIX = 0x002E, WMI_SET_FIXRATES = 0x0034,
    WMI_SET_KEEPALIVE = 0x003D, WMI_SET_WSC_STATUS = 0x0041, WMI_SET_HB_TIMEOUT = 0x0047,
    WMI_SET_FRAMERATES = 0x0048, WMI_NINTENDO_BITRATE = 0xF000,
    EV_READY = 0x1001, EV_CONNECT = 0x1002, EV_DISCONNECT = 0x1003, EV_BSSINFO = 0x1004,
    EV_CMD_ERROR = 0x1005, EV_REG_DOMAIN = 0x1006, EV_SCAN_COMPLETE = 0x100A,
};
enum { AUTH_NONE = 1, AUTH_WPA_PSK = 3, AUTH_WPA2_PSK = 5 };
enum { CRYPT_NONE = 1, CRYPT_WEP = 2, CRYPT_TKIP = 3, CRYPT_AES = 4 };
enum { BSS_FILTER_NONE = 0, BSS_FILTER_ALL = 1, BSS_FILTER_PROBED = 6 };
/* WMI_DISCONNECT_EVENT reasons (ath6kl wmi.h) */
enum { DISC_NO_NETWORK = 1, DISC_CMD = 3, DISC_BSS_DISCONNECTED = 4, DISC_ASSOC_FAILED = 6 };

#define AP_BSSID    { 0x0E, 0xA9, 0x11, 0xC0, 0xFF, 0xEE }
#define SCAN_CH_NS  (30 * SCALE_MS)         /* per channel (dwell + switching) */
#define RXQ_MAX     (24 * 1024)             /* firmware RX buffering towards the host */

typedef enum { CARD_IDLE, CARD_READY, CARD_STBY, CARD_CMD } CardState;
typedef enum { TGT_OFF, TGT_BMI, TGT_BOOTING, TGT_FW, TGT_DEAD } TgtMode;
typedef enum { ASSOC_NONE, ASSOC_PENDING, ASSOC_UP } AssocState;
typedef enum { HS_IDLE, HS_M1, HS_M3, HS_DONE, HS_GROUP } HsState;

struct CtrWifiState {
    SysBusDevice parent_obj;
    MemoryRegion tmio_mr, gpio_mr, wificnt_mr;
    NICState *nic;
    NICConf conf;

    /* properties */
    char *ssid, *psk, *security, *group;
    uint32_t channel_mhz;
    bool hidden;
    bool fifo32;                    /* false: the FIFO32 port does not move data */
    bool decoys;                    /* other (beacon-only) networks in scans */
    uint32_t rekey_s;               /* group key rekey every N s after the 4-way (0 = never) */
    uint32_t debug;

    /* 3DS side power */
    uint8_t wificnt;
    uint16_t gpio[0x30 / 2];
    bool powered;

    /* TMIO controller 2 */
    uint16_t regs[0x200 / 2];
    uint16_t stat0, stat1;
    uint16_t resp[2];
    bool xfer_rd, xfer_wr, rx_full;
    uint32_t blocks_left, blklen, pos;
    uint8_t block[512];

    /* SDIO card */
    CardState cstate;
    uint16_t rca;
    uint8_t ioe, ien, busif, pwrctl;
    uint16_t fn0_blksz, fn1_blksz;
    struct { bool active, write, inc; uint32_t func, addr, bytes; } cx;  /* CMD53 */
    uint8_t f1regs[0x100];          /* 0x400-0x4FF backing store */
    uint8_t bmi_credits;
    uint8_t err_int;

    /* target (AR6014) */
    TgtMode mode;
    uint8_t *ram;                   /* AR_RAM_BASE.. */
    uint8_t *written;               /* one flag per 256 bytes */
    uint32_t socreg_addr[32], socreg_val[32];
    int nsocregs;
    uint8_t in[4096];               /* host -> target mailbox bytes */
    uint32_t in_len;
    GByteArray *out;                /* target -> host mailbox bytes */
    uint32_t lz_start, lz_bytes, wr_bytes;
    bool lz_active, stub_ran;
    QEMUTimer *boot_timer;

    /* HTC */
    bool htc_setup_done;
    uint16_t ep_svc[8];             /* service id per endpoint, 0 = free */
    int next_ep, wmi_ep;
    uint8_t owed[8];                /* credits to return per endpoint */
    uint8_t credits[8];             /* credits granted to the host per endpoint */
    uint8_t credit_pool;            /* unassigned credits (rest go to the first data endpoint) */
    uint32_t credit_used, credit_reports, credit_overflow, credit_waits;
    QEMUTimer *credit_timer;

    /* WMI / station */
    uint8_t sta_mac[6];
    uint8_t bss_filter;
    uint8_t probed_ssid[33];
    int probed_len;
    bool scanning;
    uint32_t scan_ch_mask;          /* channels still to scan (bit n = channel n) */
    QEMUTimer *scan_timer;
    AssocState assoc;
    uint8_t conn[52];               /* the WMI_CONNECT parameters */
    int sec_mode;                   /* SEC_*: from the "security" property */
    QEMUTimer *assoc_timer;
    bool sta_tk_ok, sta_gtk_ok;     /* installed keys equal the AP's */
    bool sta_tk_set, sta_gtk_set;
    uint32_t tx_frames, rx_frames, tx_drop, rx_drop;

    /* access point */
    uint8_t bssid[6];
    bool wpa2, wpa1, group_tkip;
    uint8_t pmk[32];
    uint8_t assoc_ie[24];           /* RSN IE of the association request */
    int assoc_ie_len;
    HsState hs;
    uint8_t anonce[32], snonce[32], ptk[64], gtk[32];
    int gtk_len, gtk_idx;
    uint64_t replay;
    int hs_tries;
    bool port_open;
    QEMUTimer *hs_timer;
    QEMUTimer *rekey_timer;
    int rekeys;                     /* group rekeys completed */
};

#define WLOG(s, lvl, fmt, ...) do { \
        if ((s)->debug >= (lvl)) { \
            fprintf(stderr, "ctr-wifi: %8.3f " fmt "\n", now_ns() / 1e9, ## __VA_ARGS__); \
        } \
    } while (0)
#define GERR(fmt, ...) qemu_log_mask(LOG_GUEST_ERROR, "ctr-wifi: " fmt "\n", ## __VA_ARGS__)

static int64_t now_ns(void) { return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
static inline uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t rd16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline void wr32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void wr16le(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr16be(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline uint16_t rd16be(const uint8_t *p) { return p[0] << 8 | p[1]; }

static const char *mac_str(const uint8_t *m)
{
    static char b[4][18];
    static int i;
    i = (i + 1) & 3;
    snprintf(b[i], sizeof(b[i]), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b[i];
}

/* ======================================================================== */
/* crypto for the authenticator: GLib HMAC-SHA1, QEMU's AES                  */
/* ======================================================================== */

static void hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *const *parts,
                      const size_t *lens, int n, uint8_t out[20])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA1, key, klen);
    gsize len = 20;
    for (int i = 0; i < n; i++) {
        g_hmac_update(h, parts[i], lens[i]);
    }
    g_hmac_get_digest(h, out, &len);
    g_hmac_unref(h);
}

/* IEEE 802.11i H.4: PSK = PBKDF2(passphrase, ssid, 4096, 256) */
static void psk_to_pmk(const char *pass, const char *ssid, uint8_t pmk[32])
{
    for (uint32_t blk = 1; blk <= 2; blk++) {
        uint8_t ctr[4] = { blk >> 24, blk >> 16, blk >> 8, blk }, u[20], t[20];
        const uint8_t *p[2] = { (const uint8_t *)ssid, ctr };
        size_t l[2] = { strlen(ssid), 4 };
        hmac_sha1((const uint8_t *)pass, strlen(pass), p, l, 2, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            const uint8_t *q[1] = { u };
            size_t ql[1] = { 20 };
            uint8_t v[20];
            hmac_sha1((const uint8_t *)pass, strlen(pass), q, ql, 1, v);
            memcpy(u, v, 20);
            for (int j = 0; j < 20; j++) {
                t[j] ^= u[j];
            }
        }
        memcpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

/* IEEE 802.11i 8.5.1.2: PTK = PRF-384(PMK, "Pairwise key expansion",
   min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(ANonce,SNonce)) */
static void derive_ptk(CtrWifiState *s)
{
    static const char label[] = "Pairwise key expansion";
    uint8_t data[76], out[80];
    const uint8_t *a = s->bssid, *b = s->sta_mac;
    if (memcmp(a, b, 6) > 0) {
        const uint8_t *t = a; a = b; b = t;
    }
    memcpy(data, a, 6);
    memcpy(data + 6, b, 6);
    const uint8_t *n1 = s->anonce, *n2 = s->snonce;
    if (memcmp(n1, n2, 32) > 0) {
        const uint8_t *t = n1; n1 = n2; n2 = t;
    }
    memcpy(data + 12, n1, 32);
    memcpy(data + 44, n2, 32);
    for (uint8_t i = 0; i < (s->wpa1 ? 4 : 3); i++) {
        uint8_t zero = 0;
        const uint8_t *p[4] = { (const uint8_t *)label, &zero, data, &i };
        size_t l[4] = { strlen(label), 1, sizeof(data), 1 };
        hmac_sha1(s->pmk, 32, p, l, 4, out + 20 * i);
    }
    /* KCK 0-15, KEK 16-31, TK 32-47 (CCMP) or 32-63 (TKIP) */
    memcpy(s->ptk, out, s->wpa1 ? 64 : 48);
}

/* RFC 3394 key wrap; n = plaintext bytes (multiple of 8), out gets n + 8 */
static void key_wrap(const uint8_t kek[16], const uint8_t *in, int n, uint8_t *out)
{
    AES_KEY k;
    uint8_t a[8], b[16];
    int blocks = n / 8;
    AES_set_encrypt_key(kek, 128, &k);
    memset(a, 0xA6, 8);
    memcpy(out + 8, in, n);
    for (int j = 0; j <= 5; j++) {
        for (int i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            AES_encrypt(b, b, &k);
            uint64_t t = (uint64_t)blocks * j + i;
            memcpy(a, b, 8);
            for (int x = 0; x < 8; x++) {
                a[7 - x] ^= (uint8_t)(t >> (8 * x));
            }
            memcpy(out + 8 * i, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
}

/* WPA v1 uses HMAC-MD5 for the EAPOL-Key MIC and RC4 (dropping 256 bytes of
   keystream) for Key Data. glib provides MD5; RC4-drop is a few lines. */
static void md5_raw(const uint8_t *data, size_t n, uint8_t out[16])
{
    uint8_t *r = NULL;
    size_t len = 0;
    Error *err = NULL;
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_MD5, data, n, &r, &len, &err) == 0 && len >= 16)
        memcpy(out, r, 16);
    else
        memset(out, 0, 16);
    g_free(r);
}

static void md5_hmac16(const uint8_t key[16], const uint8_t *data, size_t n, uint8_t out[16])
{
    uint8_t k[64], buf[640], inner[16], result[16];
    size_t take = n > sizeof(buf) - 64 ? sizeof(buf) - 64 : n;
    memset(k, 0, sizeof(k));
    memcpy(k, key, 16);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
    memcpy(buf + 64, data, take);
    md5_raw(buf, 64 + take, inner);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5C;
    memcpy(buf + 64, inner, 16);
    md5_raw(buf, 80, result);
    memcpy(out, result, 16);
}

static void rc4_drop(const uint8_t key[32], uint8_t *data, int len)
{
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + s[i] + key[i & 31]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    int i = 0;
    j = 0;
    for (int k = 0; k < 256 + len; k++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        if (k >= 256) data[k - 256] ^= s[(s[i] + s[j]) & 255];
    }
}

/* ======================================================================== */
/* target -> host mailbox                                                    */
/* ======================================================================== */

static uint32_t io_block(CtrWifiState *s)
{
    uint32_t v = rd32le(s->ram + (AR_HI - AR_RAM_BASE) + HI_MBOX_IO_BLOCK_SZ);
    return (v >= 4 && v <= 512) ? v : 128;
}

/* One HTC message: header, payload, credit trailer, padded to the block size
   the host put into hi_mbox_io_block_sz. */
static void htc_queue(CtrWifiState *s, int ep, const uint8_t *body, uint32_t len)
{
    uint8_t trailer[2 + 16];
    uint32_t tl = 0, returned = 0;
    int n = 0;
    for (int e = 0; e < 8; e++) {
        if (s->owed[e]) {
            trailer[2 + 2 * n] = e;
            trailer[3 + 2 * n] = s->owed[e];
            returned += s->owed[e];
            s->owed[e] = 0;
            n++;
        }
    }
    if (n) {
        trailer[0] = HTC_REC_CREDITS;
        trailer[1] = 2 * n;
        tl = 2 + 2 * n;
        s->credit_reports += returned;
        if (!(s->credit_reports % 64)) {
            WLOG(s, 1, "HTC credits: consumed %u, returned %u, %u wait(s), %u overflow(s)",
                 s->credit_used, s->credit_reports, s->credit_waits, s->credit_overflow);
        }
    }
    uint32_t padded = ROUND_UP(6 + len + tl, io_block(s));
    uint8_t *m = g_malloc0(padded);
    m[0] = ep;
    m[1] = tl ? HTC_FLG_RX_TRAILER : 0;
    wr16le(m + 2, len + tl);
    m[4] = tl;
    if (len) {
        memcpy(m + 6, body, len);
    }
    if (tl) {
        memcpy(m + 6 + len, trailer, tl);
    }
    g_byte_array_append(s->out, m, padded);
    g_free(m);
}

static void wmi_event(CtrWifiState *s, uint16_t id, const uint8_t *p, uint32_t len)
{
    uint8_t b[512];
    wr16le(b, id);
    if (len) {
        memcpy(b + 2, p, len);
    }
    htc_queue(s, s->wmi_ep, b, len + 2);
}

/* NEED_CREDIT_UPDATE and nothing else to say: an empty EP0 message whose
   trailer returns the credits (ath6kl handles these as "credit reports") */
static void credit_flush(void *opaque)
{
    CtrWifiState *s = opaque;
    for (int e = 0; e < 8; e++) {
        if (s->owed[e] && s->mode == TGT_FW) {
            htc_queue(s, 0, NULL, 0);
            return;
        }
    }
}

/* ======================================================================== */
/* target memory and registers                                               */
/* ======================================================================== */

static bool ram_ok(uint32_t a, uint32_t n)
{
    return a >= AR_RAM_BASE && a - AR_RAM_BASE + n <= AR_RAM_SIZE;
}

static void ram_write(CtrWifiState *s, uint32_t a, const uint8_t *p, uint32_t n)
{
    if (!n || !ram_ok(a, n)) {
        return;
    }
    memcpy(s->ram + (a - AR_RAM_BASE), p, n);
    for (uint32_t x = a; x < a + n; x += 256) {
        s->written[(x - AR_RAM_BASE) >> 8] = 1;
    }
    s->written[(a + n - 1 - AR_RAM_BASE) >> 8] = 1;
}

static bool ram_written(CtrWifiState *s, uint32_t a)
{
    return ram_ok(a, 1) && s->written[(a - AR_RAM_BASE) >> 8];
}

static uint32_t *socreg(CtrWifiState *s, uint32_t a, bool create)
{
    for (int i = 0; i < s->nsocregs; i++) {
        if (s->socreg_addr[i] == a) {
            return &s->socreg_val[i];
        }
    }
    if (!create || s->nsocregs >= 32) {
        return NULL;
    }
    s->socreg_addr[s->nsocregs] = a;
    s->socreg_val[s->nsocregs] = 0;
    return &s->socreg_val[s->nsocregs++];
}

static void target_reset(CtrWifiState *s, bool cold);

static uint32_t target_read32(CtrWifiState *s, uint32_t a)
{
    if (ram_ok(a, 4)) {
        return rd32le(s->ram + (a - AR_RAM_BASE));
    }
    if (a == SOC_CHIP_ID) {
        return AR_CHIP_ID;
    }
    uint32_t *r = socreg(s, a, false);
    return r ? *r : 0;
}

static void target_write32(CtrWifiState *s, uint32_t a, uint32_t v)
{
    if (ram_ok(a, 4)) {
        uint8_t b[4];
        wr32le(b, v);
        ram_write(s, a, b, 4);
        return;
    }
    if (a == SOC_RESET_CONTROL && (v & 0x100)) {
        WLOG(s, 1, "warm reset into the BMI ROM (RESET_CONTROL = %08x)", v);
        target_reset(s, false);
        return;
    }
    if (a == SOC_CHIP_ID) {
        return;
    }
    uint32_t *r = socreg(s, a, true);
    if (r) {
        *r = v;
    }
}

/* ======================================================================== */
/* BMI (the ROM bootloader's command stream on mailbox 0)                    */
/* ======================================================================== */

static void bmi_reply32(CtrWifiState *s, uint32_t v)
{
    uint8_t b[4];
    wr32le(b, v);
    g_byte_array_append(s->out, b, 4);
}

static void fw_boot(void *opaque)
{
    CtrWifiState *s = opaque;
    uint32_t dset = rd32le(s->ram + (AR_HI - AR_RAM_BASE) + HI_DSET_LIST_HEAD);
    const char *why = NULL;
    if (s->lz_bytes < 1024) {
        why = "no main firmware was streamed (BMI_LZ_DATA)";
    } else if (!s->stub_ran) {
        why = "the board-data stub was never executed";
    } else if (!dset || !ram_written(s, dset)) {
        why = "hi_dset_list_head does not point at an uploaded database";
    }
    if (why) {
        s->mode = TGT_DEAD;
        WLOG(s, 1, "firmware does not start: %s (target is dead until reset)", why);
        return;
    }
    s->mode = TGT_FW;
    memset(s->ep_svc, 0, sizeof(s->ep_svc));
    s->ep_svc[0] = 0x0001;                  /* HTC control */
    s->next_ep = 1;
    s->wmi_ep = -1;
    /* HTC_READY: id, credit count, credit size, max endpoints */
    uint8_t r[8] = { 1, 0 };
    wr16le(r + 2, HTC_CREDITS);
    wr16le(r + 4, HTC_CREDIT_SIZE);
    r[6] = 6;
    htc_queue(s, 0, r, sizeof(r));
    WLOG(s, 1, "firmware running (main %u LZ bytes, %u RAM bytes uploaded, block size %u): HTC_READY",
         s->lz_bytes, s->wr_bytes, io_block(s));
}

/* Execute every complete BMI command in s->in. */
static void bmi_feed(CtrWifiState *s)
{
    while (s->in_len >= 4 && s->mode == TGT_BMI) {
        uint8_t *p = s->in;
        uint32_t cmd = rd32le(p), need, dlen = 0;
        switch (cmd) {
        case BMI_NOP: case BMI_DONE: case BMI_GET_TARGET_ID:
            need = 4; break;
        case BMI_SET_APP_START: case BMI_READ_SOC_REG: case BMI_LZ_START:
            need = 8; break;
        case BMI_READ_MEMORY: case BMI_EXECUTE: case BMI_WRITE_SOC_REG:
            need = 12; break;
        case BMI_WRITE_MEMORY:
            if (s->in_len < 12) {
                return;
            }
            dlen = rd32le(p + 8);
            need = 12 + dlen;
            break;
        case BMI_LZ_DATA:
            if (s->in_len < 8) {
                return;
            }
            dlen = rd32le(p + 4);
            need = 8 + dlen;
            break;
        default:
            WLOG(s, 1, "BMI: unknown command %08x: ROM wedged", cmd);
            s->mode = TGT_DEAD;
            return;
        }
        if (dlen > BMI_DATA_MAX) {
            WLOG(s, 1, "BMI: command %u with %u data bytes (ROM buffer is %u): ROM wedged",
                 cmd, dlen, BMI_DATA_MAX);
            s->mode = TGT_DEAD;
            return;
        }
        if (s->in_len < need) {
            return;
        }
        uint32_t a1 = need >= 8 ? rd32le(p + 4) : 0, a2 = need >= 12 ? rd32le(p + 8) : 0;
        switch (cmd) {
        case BMI_DONE:
            WLOG(s, 1, "BMI_DONE: starting the firmware");
            s->mode = TGT_BOOTING;
            timer_mod(s->boot_timer, now_ns() + 50 * SCALE_MS);
            break;
        case BMI_GET_TARGET_ID:
            WLOG(s, 1, "BMI_GET_TARGET_ID -> ROM version %08x", AR_ROM_VERSION);
            bmi_reply32(s, AR_ROM_VERSION);
            break;
        case BMI_READ_SOC_REG:
            bmi_reply32(s, target_read32(s, a1));
            break;
        case BMI_WRITE_SOC_REG:
            WLOG(s, 2, "BMI: SOC[%08x] = %08x", a1, a2);
            target_write32(s, a1, a2);
            break;
        case BMI_READ_MEMORY:
            if (a2 > BMI_DATA_MAX || !ram_ok(a1, a2)) {
                WLOG(s, 1, "BMI_READ_MEMORY %08x+%x outside RAM: ROM wedged", a1, a2);
                s->mode = TGT_DEAD;
                return;
            }
            g_byte_array_append(s->out, s->ram + (a1 - AR_RAM_BASE), a2);
            break;
        case BMI_WRITE_MEMORY:
            if (!ram_ok(a1, dlen)) {
                WLOG(s, 1, "BMI_WRITE_MEMORY %08x+%x outside RAM: ROM wedged", a1, dlen);
                s->mode = TGT_DEAD;
                return;
            }
            ram_write(s, a1, p + 12, dlen);
            if (a1 >= AR_HI && a1 < AR_HI + 0x100) {
                WLOG(s, 2, "BMI: host interest +%02x = %08x", a1 - AR_HI, dlen >= 4 ? rd32le(p + 12) : 0);
            } else {
                s->wr_bytes += dlen;
            }
            break;
        case BMI_EXECUTE: {
            uint32_t code = a1 >= 0x900000 ? a1 - AR_IRAM_MIRROR : a1;
            if (!ram_written(s, code)) {
                WLOG(s, 1, "BMI_EXECUTE %08x: nothing was uploaded there, the ROM jumps into "
                     "garbage (no reply)", a1);
                s->mode = TGT_DEAD;
                return;
            }
            /* The stub reads the calibration EEPROM and reports it in the host
               interest area (GBATEK "DSi Wifi Firmware"; model assumption) */
            s->stub_ran = true;
            target_write32(s, AR_HI + HI_BOARD_DATA, 0x0053F000);
            target_write32(s, AR_HI + HI_BOARD_DATA_INIT, 1);
            WLOG(s, 1, "BMI_EXECUTE %08x (arg %08x): board-data stub ran", a1, a2);
            bmi_reply32(s, 0);
            break;
        }
        case BMI_SET_APP_START: case BMI_NOP:
            break;
        case BMI_LZ_START:
            s->lz_active = true;
            s->lz_start = a1;
            WLOG(s, 2, "BMI_LZ_STREAM_START %08x", a1);
            break;
        case BMI_LZ_DATA:
            if (!s->lz_active) {
                WLOG(s, 1, "BMI_LZ_DATA without BMI_LZ_STREAM_START: ROM wedged");
                s->mode = TGT_DEAD;
                return;
            }
            s->lz_bytes += dlen;
            if (ram_ok(s->lz_start, 4)) {
                s->written[(s->lz_start - AR_RAM_BASE) >> 8] = 1;
            }
            break;
        }
        s->bmi_credits++;                   /* the ROM takes the next command */
        memmove(s->in, s->in + need, s->in_len - need);
        s->in_len -= need;
    }
}

/* ======================================================================== */
/* access point side                                                         */
/* ======================================================================== */

static int channel_of(uint32_t mhz)
{
    return (mhz >= 2412 && mhz <= 2472) ? (int)(mhz - 2407) / 5 : (mhz == 2484 ? 14 : 0);
}

/* the AP's own RSN IE (beacon, probe response, message 3): CCMP pairwise,
   PSK, capabilities 0x000C (16 PTKSA replay counters, as hostapd with WMM) */
enum { SEC_WPA2 = 0, SEC_MIXED, SEC_WPA3, SEC_WPA1 };

static int ap_rsn_ie(CtrWifiState *s, uint8_t *o)
{
    static const uint8_t ie[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                    1, 0, 0x00, 0x0F, 0xAC, 2, 0x0C, 0x00 };
    /* security=wpa2-wpa3: as a FRITZ!Box in "WPA2 + WPA3" mode announces it
       (AKM PSK + SAE, MFPC, PMKID count 0) */
    static const uint8_t mixed[28] = { 0x30, 26, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                       2, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x0F, 0xAC, 8, 0x8C, 0x00, 0, 0 };
    /* security=wpa3: SAE only, PMF required */
    static const uint8_t wpa3[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                      1, 0, 0x00, 0x0F, 0xAC, 8, 0xCC, 0x00 };
    /* security=wpa1: the WPA Alliance vendor IE, version 1, TKIP everywhere.
       Group cipher suite at offset 10, pairwise/AKM suite type at offset 17/23. */
    static const uint8_t wpa1[26] = {
        0xDD, 24, 0x00, 0x50, 0xF2, 0x01, 0x01, 0x00,
        0x00, 0x50, 0xF2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xF2, 0x02,
        0x01, 0x00, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x00,
    };
    if (s->sec_mode == SEC_WPA1) {
        memcpy(o, wpa1, sizeof(wpa1));
        return sizeof(wpa1);
    }
    const uint8_t *src = s->sec_mode == SEC_MIXED ? mixed : s->sec_mode == SEC_WPA3 ? wpa3 : ie;
    int n = src[1] + 2;
    memcpy(o, src, n);
    o[7] = s->group_tkip ? 2 : 4;
    return n;
}

/* Other networks "on the air" (beacons only): they make a scan list look
   like a real one. They never answer an association (no network). */
static const struct {
    const char *ssid;
    uint8_t bssid[6];
    uint32_t mhz;
    bool wpa2;
    int16_t rssi;
} decoys[] = {
    { "NeighborNet", { 0x0E, 0xA9, 0x11, 0x00, 0x00, 0x01 }, 2412, true, -81 },
    { "CoffeeShop", { 0x0E, 0xA9, 0x11, 0x00, 0x00, 0x02 }, 2462, false, -72 },
};

static int bss_ies(uint8_t *o, const char *ssid, int ssid_len, uint32_t mhz, const uint8_t *rsn)
{
    static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    int n = 0;
    o[n++] = 0;
    o[n++] = ssid_len;
    memcpy(o + n, ssid, ssid_len);
    n += ssid_len;
    memcpy(o + n, rates, sizeof(rates));
    n += sizeof(rates);
    o[n++] = 3;
    o[n++] = 1;
    o[n++] = channel_of(mhz);
    if (rsn) {
        memcpy(o + n, rsn, rsn[1] + 2);
        n += rsn[1] + 2;
    }
    return n;
}

static void send_bss(CtrWifiState *s, const char *ssid, int ssid_len, const uint8_t *bssid,
                     uint32_t mhz, const uint8_t *rsn, int16_t rssi, bool probe_resp)
{
    uint8_t b[256];
    memset(b, 0, 28);
    wr16le(b, mhz);
    b[2] = probe_resp ? 2 : 1;              /* frame type: beacon 1, probe response 2 */
    b[3] = rssi + 95;                       /* SNR over a -95 dBm noise floor (dsiwifi ignores < 0x20) */
    wr16le(b + 4, (uint16_t)rssi);          /* RSSI dBm */
    memcpy(b + 6, bssid, 6);
    /* 802.11 body: timestamp, beacon interval, capability (ESS, privacy) */
    wr16le(b + 16 + 8, 100);
    wr16le(b + 16 + 10, 0x0401 | (rsn ? 0x10 : 0));
    int n = 28 + bss_ies(b + 28, ssid, ssid_len, mhz, rsn);
    wmi_event(s, EV_BSSINFO, b, n);
}

static void send_bssinfo(CtrWifiState *s, bool probe_resp)
{
    uint8_t rsn[40];
    if (s->wpa2) {
        ap_rsn_ie(s, rsn);
    }
    send_bss(s, s->ssid, (s->hidden && !probe_resp) ? 0 : strlen(s->ssid), s->bssid, s->channel_mhz,
             s->wpa2 ? rsn : NULL, -55, probe_resp);
    WLOG(s, 1, "scan: BSSINFO \"%s\" %s ch %d (%s)", s->ssid, mac_str(s->bssid),
         channel_of(s->channel_mhz), probe_resp ? "probe response" : "beacon");
}

static bool ssid_probed(CtrWifiState *s)
{
    return s->probed_len == (int)strlen(s->ssid) && !memcmp(s->probed_ssid, s->ssid, s->probed_len);
}

static void scan_step(void *opaque)
{
    CtrWifiState *s = opaque;
    if (!s->scanning || s->mode != TGT_FW || !s->scan_ch_mask) {
        return;
    }
    int ch = ctz32(s->scan_ch_mask);
    s->scan_ch_mask &= ~(1u << ch);
    if (ch == channel_of(s->channel_mhz)) {
        bool probe = ssid_probed(s);
        bool visible = !s->hidden || probe;
        bool report = s->bss_filter == BSS_FILTER_ALL ||
                      (s->bss_filter == BSS_FILTER_PROBED && probe);
        if (visible && report) {
            send_bssinfo(s, probe);
        } else if (visible) {
            WLOG(s, 1, "scan: AP seen on channel %d, but BSS filter %u reports nothing", ch, s->bss_filter);
        }
    }
    for (int i = 0; s->decoys && i < (int)ARRAY_SIZE(decoys); i++) {
        if (channel_of(decoys[i].mhz) == ch && s->bss_filter == BSS_FILTER_ALL) {
            static const uint8_t rsn[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F,
                                             0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0x0C, 0x00 };
            send_bss(s, decoys[i].ssid, strlen(decoys[i].ssid), decoys[i].bssid, decoys[i].mhz,
                     decoys[i].wpa2 ? rsn : NULL, decoys[i].rssi, false);
        }
    }
    if (s->scan_ch_mask) {
        timer_mod(s->scan_timer, now_ns() + SCAN_CH_NS);
    } else {
        uint8_t st[4] = { 0 };
        s->scanning = false;
        wmi_event(s, EV_SCAN_COMPLETE, st, 4);
        WLOG(s, 1, "scan complete");
    }
}

static void disconnect_event(CtrWifiState *s, uint8_t reason, uint16_t proto)
{
    uint8_t d[10] = { 0 };
    wr16le(d, proto);
    memcpy(d + 2, s->bssid, 6);
    d[8] = reason;
    s->assoc = ASSOC_NONE;
    s->hs = HS_IDLE;
    s->port_open = false;
    timer_del(s->hs_timer);
    timer_del(s->rekey_timer);
    wmi_event(s, EV_DISCONNECT, d, sizeof(d));
    WLOG(s, 1, "WMI_DISCONNECT_EVENT: reason %u (802.11 reason/status %u)", reason, proto);
}

/* the RSN IE the firmware puts into the association request, built from
   WMI_CONNECT: group from groupCryptoType, CCMP pairwise, PSK, capabilities
   0 (model assumption: the real firmware's capability bits are unknown) */
static void build_assoc_ie(CtrWifiState *s)
{
    static const uint8_t ie[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                    1, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x00 };
    if (s->sec_mode == SEC_WPA1) {
        s->assoc_ie_len = ap_rsn_ie(s, s->assoc_ie);
        return;
    }
    memcpy(s->assoc_ie, ie, 22);
    s->assoc_ie[7] = s->conn[5] == CRYPT_TKIP ? 2 : 4;
    s->assoc_ie_len = 22;
}

static void eapol_send(CtrWifiState *s, const uint8_t *body, int blen);

static void assoc_done(void *opaque)
{
    CtrWifiState *s = opaque;
    const uint8_t *c = s->conn;
    static const uint8_t zero6[6];
    if (s->assoc != ASSOC_PENDING || s->mode != TGT_FW) {
        return;
    }
    int slen = c[7];
    uint16_t ch = rd16le(c + 40);
    if (slen != (int)strlen(s->ssid) || memcmp(c + 8, s->ssid, slen)) {
        WLOG(s, 1, "connect: SSID \"%.*s\" is not on the air", slen > 32 ? 32 : slen, (const char *)c + 8);
        disconnect_event(s, DISC_NO_NETWORK, 0);
        return;
    }
    if ((ch && ch != s->channel_mhz) || (memcmp(c + 42, zero6, 6) && memcmp(c + 42, s->bssid, 6))) {
        WLOG(s, 1, "connect: channel %u / BSSID %s do not match the AP (%u, %s)",
             ch, mac_str(c + 42), s->channel_mhz, mac_str(s->bssid));
        disconnect_event(s, DISC_NO_NETWORK, 0);
        return;
    }
    if (c[0] != 1 || c[1] != 1) {
        WLOG(s, 1, "connect: network type %u / 802.11 auth %u not supported by the AP", c[0], c[1]);
        disconnect_event(s, DISC_ASSOC_FAILED, 13);
        return;
    }
    if (s->sec_mode == SEC_WPA3) {
        WLOG(s, 1, "connect: the AP is WPA3-only (SAE, PMF required): this firmware can't join it");
        disconnect_event(s, DISC_NO_NETWORK, 0);
        return;
    }
    if (s->sec_mode == SEC_MIXED && !(rd16le(c + 48) & 0x0008)) {
        /* what the real AR6014 is suspected to do with a WPA2+WPA3 RSN IE
           (hardware run 22): its own profile match drops the BSS unless the
           host says CONNECT_PROFILE_MATCH_DONE */
        WLOG(s, 1, "connect: WPA2+WPA3 RSN IE and no CONNECT_PROFILE_MATCH_DONE: no network (as the AR6014 firmware)");
        disconnect_event(s, DISC_NO_NETWORK, 0);
        return;
    }
    if (s->sec_mode == SEC_WPA1) {
        if (c[2] != AUTH_WPA_PSK || c[3] != CRYPT_TKIP || c[5] != CRYPT_TKIP) {
            WLOG(s, 1, "connect: auth %u pairwise %u group %u; the AP needs WPA-PSK (3), TKIP (3)",
                 c[2], c[3], c[5]);
            disconnect_event(s, DISC_ASSOC_FAILED, 43);
            return;
        }
        build_assoc_ie(s);
    } else if (s->wpa2) {
        uint8_t want_group = s->group_tkip ? CRYPT_TKIP : CRYPT_AES;
        if (c[2] != AUTH_WPA2_PSK || c[3] != CRYPT_AES || c[5] != want_group) {
            WLOG(s, 1, "connect: auth %u pairwise %u group %u; the AP needs WPA2-PSK (5), AES (4), group %u",
                 c[2], c[3], c[5], want_group);
            disconnect_event(s, DISC_ASSOC_FAILED, c[5] != want_group ? 41 : c[3] != CRYPT_AES ? 42 : 43);
            return;
        }
        build_assoc_ie(s);
    } else if (c[2] != AUTH_NONE || c[3] != CRYPT_NONE) {
        WLOG(s, 1, "connect: auth %u / crypto %u, but the AP is open", c[2], c[3]);
        disconnect_event(s, DISC_ASSOC_FAILED, 43);
        return;
    }
    s->assoc = ASSOC_UP;
    s->sta_tk_set = s->sta_gtk_set = s->sta_tk_ok = s->sta_gtk_ok = false;
    /* WMI_CONNECT_EVENT (AR6K wmi.h): channel, bssid, listen and beacon
       interval, network type, the three lengths, then assoc request/response */
    uint8_t e[160];
    wr16le(e, s->channel_mhz);
    memcpy(e + 2, s->bssid, 6);
    wr16le(e + 8, 100);
    wr16le(e + 10, 100);
    wr32le(e + 12, 1);
    uint8_t *lens = e + 16;
    int n = 19;
    lens[0] = 0;                                    /* beacon IEs: none */
    int req = n;
    wr16le(e + n, 0x0401 | (s->wpa2 ? 0x10 : 0));   /* capability */
    wr16le(e + n + 2, 1);                           /* listen interval */
    n += 4;
    e[n++] = 0;
    e[n++] = slen;
    memcpy(e + n, s->ssid, slen);
    n += slen;
    if (s->wpa2) {
        memcpy(e + n, s->assoc_ie, s->assoc_ie_len);
        n += s->assoc_ie_len;
    }
    lens[1] = n - req;
    int resp = n;
    wr16le(e + n, 0x0401 | (s->wpa2 ? 0x10 : 0));
    wr16le(e + n + 2, 0);                           /* status: success */
    wr16le(e + n + 4, 0xC001);                      /* AID 1 */
    n += 6;
    lens[2] = n - resp;
    wmi_event(s, EV_CONNECT, e, n);
    WLOG(s, 1, "associated with \"%s\" (%s) on %u MHz, station %s%s", s->ssid, mac_str(s->bssid),
         s->channel_mhz, mac_str(s->sta_mac), s->wpa2 ? ": starting the 4-way handshake" : "");
    if (s->wpa2) {
        s->hs_tries = 0;
        s->hs = HS_IDLE;
        for (int i = 0; i < 32; i++) {
            s->anonce[i] = g_random_int_range(0, 256);
        }
        timer_mod(s->hs_timer, now_ns() + 10 * SCALE_MS);
    } else {
        s->port_open = true;
    }
}

/* ---- authenticator (IEEE 802.11i 8.5.3), offsets from the 802.1X header -- */
#define K_INFO 5
#define K_REPLAY 9
#define K_NONCE 17
#define K_IV 49                             /* WPA v1 RC4 Key IV */
#define K_MIC 81
#define K_DLEN 97
#define K_DATA 99
#define KI_V2 0x0002
#define KI_PAIRWISE 0x0008
#define KI_INSTALL 0x0040
#define KI_ACK 0x0080
#define KI_MIC 0x0100
#define KI_SECURE 0x0200
#define KI_ENC 0x1000

static void eapol_mic(CtrWifiState *s, const uint8_t *f, int len, uint8_t mic[16])
{
    uint8_t *tmp = g_memdup2(f, len), d[20];
    memset(tmp + K_MIC, 0, 16);
    if (s->wpa1) {
        md5_hmac16(s->ptk, tmp, len, mic);
    } else {
        const uint8_t *p[1] = { tmp };
        size_t l[1] = { (size_t)len };
        hmac_sha1(s->ptk, 16, p, l, 1, d);
        memcpy(mic, d, 16);
    }
    g_free(tmp);
}

static inline uint16_t key_ver(CtrWifiState *s) { return s->wpa1 ? 0 : KI_V2; }

static void eapol_key(CtrWifiState *s, uint16_t info, const uint8_t *data, int dlen, bool mic)
{
    uint8_t f[256];
    int len = K_DATA + dlen;
    memset(f, 0, len);
    f[0] = s->wpa1 ? 1 : 2;                         /* WPA v1: 802.1X-2001 */
    f[1] = 3;                                       /* EAPOL-Key */
    wr16be(f + 2, len - 4);
    f[4] = s->wpa1 ? 0xFE : 2;                      /* WPA vendor / RSN descriptor */
    wr16be(f + K_INFO, info);
    wr16be(f + 7, s->wpa1 ? 32 : 16);               /* TKIP / CCMP key length */
    for (int i = 0; i < 8; i++) {
        f[K_REPLAY + i] = s->replay >> (56 - 8 * i);
    }
    memcpy(f + K_NONCE, s->anonce, 32);
    wr16be(f + K_DLEN, dlen);
    if (dlen) {
        memcpy(f + K_DATA, data, dlen);
        if (s->wpa1 && (info & KI_ENC)) {
            uint8_t ek[32];
            for (int i = 0; i < 16; i++) f[K_IV + i] = g_random_int_range(0, 256);
            memcpy(ek, f + K_IV, 16);
            memcpy(ek + 16, s->ptk + 16, 16);
            rc4_drop(ek, f + K_DATA, dlen);
        }
    }
    if (mic) {
        eapol_mic(s, f, len, f + K_MIC);
    }
    eapol_send(s, f, len);
}

static void hs_send_m1(CtrWifiState *s)
{
    s->replay++;
    s->hs = HS_M1;
    eapol_key(s, key_ver(s) | KI_PAIRWISE | KI_ACK, NULL, 0, false);
    WLOG(s, 1, "4-way: message 1 sent (try %d)", s->hs_tries + 1);
}

static void hs_send_m3(CtrWifiState *s)
{
    uint8_t kd[104], wrapped[112];
    const uint8_t *keydata;
    int keylen, n = ap_rsn_ie(s, kd);
    kd[n++] = 0xDD;                                 /* GTK KDE */
    kd[n++] = 6 + s->gtk_len;
    kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 0x01;
    kd[n++] = s->gtk_idx;
    kd[n++] = 0;
    memcpy(kd + n, s->gtk, s->gtk_len);
    n += s->gtk_len;
    if (n % 8) {                                    /* key data padding: DD 00 .. */
        kd[n++] = 0xDD;
        while (n % 8) {
            kd[n++] = 0;
        }
    }
    if (s->wpa1) {
        keydata = kd;                              /* WPA v1: RC4, no 8-byte wrap */
        keylen = n;
    } else {
        key_wrap(s->ptk + 16, kd, n, wrapped);
        keydata = wrapped;
        keylen = n + 8;
    }
    s->replay++;
    s->hs = HS_M3;
    uint16_t info = key_ver(s) | KI_PAIRWISE | KI_ACK | KI_MIC | KI_SECURE | KI_ENC;
    info |= s->wpa1 ? (uint16_t)((s->gtk_idx & 3) << 5) : KI_INSTALL;
    eapol_key(s, info, keydata, keylen, true);
    WLOG(s, 1, "4-way: message 3 sent (GTK index %d, %d key data bytes)", s->gtk_idx, keylen);
}

/* group key handshake message 1 (rekey): new GTK, other key index */
static void hs_send_g1(CtrWifiState *s)
{
    uint8_t kd[48], wrapped[56];
    const uint8_t *keydata;
    int keylen, n = 0;
    kd[n++] = 0xDD;                                 /* GTK KDE */
    kd[n++] = 6 + s->gtk_len;
    kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 0x01;
    kd[n++] = s->gtk_idx;
    kd[n++] = 0;
    memcpy(kd + n, s->gtk, s->gtk_len);
    n += s->gtk_len;
    if (n % 8) {
        kd[n++] = 0xDD;
        while (n % 8) {
            kd[n++] = 0;
        }
    }
    if (s->wpa1) {
        keydata = kd;
        keylen = n;
    } else {
        key_wrap(s->ptk + 16, kd, n, wrapped);
        keydata = wrapped;
        keylen = n + 8;
    }
    s->replay++;
    s->hs = HS_GROUP;
    eapol_key(s, key_ver(s) | KI_ACK | KI_MIC | KI_SECURE | KI_ENC, keydata, keylen, true);
    WLOG(s, 1, "group rekey: message 1 sent (GTK index %d, try %d)", s->gtk_idx, s->hs_tries + 1);
}

static void rekey_timeout(void *opaque)
{
    CtrWifiState *s = opaque;
    if (s->assoc != ASSOC_UP || s->hs != HS_DONE) {
        return;
    }
    s->gtk_idx = s->gtk_idx == 1 ? 2 : 1;
    for (int i = 0; i < s->gtk_len; i++) {
        s->gtk[i] = g_random_int_range(0, 256);
    }
    s->sta_gtk_ok = false;
    s->hs_tries = 0;
    hs_send_g1(s);
    timer_mod(s->hs_timer, now_ns() + 1000 * SCALE_MS);
}

static void hs_timeout(void *opaque)
{
    CtrWifiState *s = opaque;
    if (s->assoc != ASSOC_UP || s->hs == HS_DONE) {
        return;
    }
    if (s->hs == HS_GROUP) {
        if (++s->hs_tries >= 4) {
            /* what a FRITZ!Box did in hardware run 23 */
            WLOG(s, 1, "group rekey: no answer after 4 tries: deauthenticating (reason 16, group key handshake timeout)");
            disconnect_event(s, DISC_BSS_DISCONNECTED, 16);
            return;
        }
        hs_send_g1(s);
        timer_mod(s->hs_timer, now_ns() + 1000 * SCALE_MS);
        return;
    }
    if (s->hs != HS_IDLE && ++s->hs_tries >= 4) {
        WLOG(s, 1, "4-way: no valid answer after 4 tries: deauthenticating (reason 15)");
        disconnect_event(s, DISC_BSS_DISCONNECTED, 15);
        return;
    }
    if (s->hs == HS_M3) {
        hs_send_m3(s);
    } else {
        hs_send_m1(s);
    }
    timer_mod(s->hs_timer, now_ns() + 1000 * SCALE_MS);
}

/* hostapd wpa_compare_rsn_ie: identical, or the same parsed suites + caps */
static bool rsn_equal(const uint8_t *a, int al, const uint8_t *b, int bl)
{
    if (al == bl && !memcmp(a, b, al)) {
        return true;
    }
    if (al < 22 || bl < 22 || a[0] != 0x30 || b[0] != 0x30) {
        return false;
    }
    return !memcmp(a + 4, b + 4, 4) && !memcmp(a + 10, b + 10, 4) && !memcmp(a + 16, b + 16, 4) &&
           !memcmp(a + 20, b + 20, 2);
}

static void ap_rx_eapol(CtrWifiState *s, const uint8_t *f, int len)
{
    if (len < K_DATA || f[1] != 3 || f[4] != (s->wpa1 ? 0xFE : 2)) {
        WLOG(s, 1, "4-way: ignoring a non-RSN EAPOL frame (%d bytes)", len);
        return;
    }
    int body = rd16be(f + 2) + 4;
    if (body > len || body < K_DATA) {
        WLOG(s, 1, "4-way: EAPOL length %d does not fit the frame (%d)", body, len);
        return;
    }
    len = body;
    uint16_t info = rd16be(f + K_INFO);
    uint64_t rc = 0;
    for (int i = 0; i < 8; i++) {
        rc = rc << 8 | f[K_REPLAY + i];
    }
    int dlen = rd16be(f + K_DLEN);
    if (s->hs == HS_GROUP && (info & 7) == key_ver(s) && !(info & (KI_PAIRWISE | KI_ACK | KI_INSTALL)) &&
        (info & KI_MIC) && (info & KI_SECURE)) {
        uint8_t gm[16];
        if (rc != s->replay) {
            WLOG(s, 1, "group rekey: replay counter %" PRIu64 ", expected %" PRIu64, rc, s->replay);
            return;
        }
        eapol_mic(s, f, len, gm);
        if (memcmp(gm, f + K_MIC, 16)) {
            WLOG(s, 1, "group rekey: message 2 MIC is wrong");
            return;
        }
        s->hs = HS_DONE;
        s->rekeys++;
        timer_del(s->hs_timer);
        WLOG(s, 1, "group rekey: message 2 OK (rekey %d)", s->rekeys);
        if (s->rekey_s) {
            timer_mod(s->rekey_timer, now_ns() + (int64_t)s->rekey_s * 1000 * SCALE_MS);
        }
        return;
    }
    if ((info & 7) != key_ver(s) || !(info & KI_PAIRWISE) || (info & (KI_ACK | KI_INSTALL)) || !(info & KI_MIC)) {
        WLOG(s, 1, "4-way: unexpected key info %04x", info);
        return;
    }
    if (rc != s->replay) {
        WLOG(s, 1, "4-way: replay counter %" PRIu64 ", expected %" PRIu64, rc, s->replay);
        return;
    }
    uint8_t mic[16];
    if (s->hs == HS_M1 && !(info & KI_SECURE)) {
        memcpy(s->snonce, f + K_NONCE, 32);
        derive_ptk(s);
        eapol_mic(s, f, len, mic);
        if (memcmp(mic, f + K_MIC, 16)) {
            WLOG(s, 1, "4-way: message 2 MIC is wrong (wrong passphrase?)");
            return;
        }
        if (K_DATA + dlen > len || (s->wpa1 ?
            (dlen != s->assoc_ie_len || memcmp(f + K_DATA, s->assoc_ie, dlen)) :
            !rsn_equal(f + K_DATA, dlen, s->assoc_ie, s->assoc_ie_len))) {
            WLOG(s, 1, "4-way: message 2 RSN IE differs from the association request: deauth");
            disconnect_event(s, DISC_BSS_DISCONNECTED, 17);
            return;
        }
        WLOG(s, 1, "4-way: message 2 OK (MIC, RSN IE)");
        s->gtk_len = (s->wpa1 || s->group_tkip) ? 32 : 16;
        s->gtk_idx = 1;
        for (int i = 0; i < s->gtk_len; i++) {
            s->gtk[i] = g_random_int_range(0, 256);
        }
        s->hs_tries = 0;
        hs_send_m3(s);
        timer_mod(s->hs_timer, now_ns() + 1000 * SCALE_MS);
    } else if (s->hs == HS_M3 && (info & KI_SECURE)) {
        eapol_mic(s, f, len, mic);
        if (memcmp(mic, f + K_MIC, 16)) {
            WLOG(s, 1, "4-way: message 4 MIC is wrong");
            return;
        }
        s->hs = HS_DONE;
        s->port_open = true;
        timer_del(s->hs_timer);
        WLOG(s, 1, "4-way: message 4 OK: AP installed the PTK, 802.1X port open");
        if (s->rekey_s) {
            timer_mod(s->rekey_timer, now_ns() + (int64_t)s->rekey_s * 1000 * SCALE_MS);
        }
    } else {
        WLOG(s, 1, "4-way: EAPOL-Key %04x in state %d ignored", info, s->hs);
    }
}

/* ======================================================================== */
/* data path                                                                 */
/* ======================================================================== */

/* AP -> station: an Ethernet II frame, handed to the host by the firmware as
   WMI data (rssi, info, 802.3 header, LLC/SNAP) on the best-effort endpoint */
static bool deliver_to_sta(CtrWifiState *s, const uint8_t *eth, int len, bool eapol)
{
    if (s->mode != TGT_FW || s->assoc != ASSOC_UP) {
        return false;
    }
    bool group = eth[0] & 1;
    if (!eapol && s->wpa2) {
        if (!s->port_open) {
            return false;
        }
        /* the firmware decrypts with whatever keys the host installed */
        if (group ? !s->sta_gtk_ok : !s->sta_tk_ok) {
            s->rx_drop++;
            if (s->rx_drop < 4 || !(s->rx_drop % 100)) {
                WLOG(s, 1, "rx: %s frame cannot be decrypted by the firmware: %s key %s",
                     group ? "group" : "unicast", group ? "group" : "pairwise",
                     (group ? s->sta_gtk_set : s->sta_tk_set) ? "differs from the AP's" : "not installed");
            }
            return false;
        }
    }
    if (s->out->len > RXQ_MAX) {
        s->rx_drop++;
        return false;
    }
    int ep = -1;
    for (int e = 0; e < 8; e++) {
        if (s->ep_svc[e] == 0x0101) {
            ep = e;
        }
    }
    int pl = len - 14;
    if (ep < 0 || pl < 0 || pl > 1500) {
        return false;
    }
    uint8_t m[2 + 14 + 8 + 1500];
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    m[0] = 40;                                      /* rssi */
    m[1] = 0;                                       /* info: data, UP 0 */
    memcpy(m + 2, eth, 12);
    wr16be(m + 14, 8 + pl);
    memcpy(m + 16, snap, 6);
    memcpy(m + 22, eth + 12, 2);
    memcpy(m + 24, eth + 14, pl);
    htc_queue(s, ep, m, 24 + pl);
    if (!eapol) {
        s->rx_frames++;
    }
    return true;
}

static void eapol_send(CtrWifiState *s, const uint8_t *body, int blen)
{
    uint8_t eth[14 + 256];
    memcpy(eth, s->sta_mac, 6);
    memcpy(eth + 6, s->bssid, 6);
    eth[12] = 0x88;
    eth[13] = 0x8E;
    memcpy(eth + 14, body, blen);
    deliver_to_sta(s, eth, 14 + blen, true);
}

/* station -> AP: WMI data from the host on a data endpoint */
static void sta_tx(CtrWifiState *s, const uint8_t *d, uint32_t len)
{
    if (len < 2 + 14) {
        WLOG(s, 1, "tx: %u-byte data message dropped", len);
        return;
    }
    if ((d[1] & 3) != 0) {
        WLOG(s, 1, "tx: WMI data header %02x %02x is not a data message", d[0], d[1]);
        return;
    }
    const uint8_t *f = d + 2;
    uint32_t tl = rd16be(f + 12), flen = len - 2;
    uint8_t eth[1514];
    if (tl >= 0x600) {
        WLOG(s, 1, "tx: Ethernet II frame from the host; the firmware wants 802.3 + LLC/SNAP: dropped");
        s->tx_drop++;
        return;
    }
    if (tl > flen - 14 || tl < 8 || tl - 8 > 1500 || f[14] != 0xAA || f[15] != 0xAA || f[16] != 0x03) {
        WLOG(s, 1, "tx: bad 802.3 length %u (frame %u) or LLC header: dropped", tl, flen);
        s->tx_drop++;
        return;
    }
    int elen = 14 + tl - 8;
    memcpy(eth, f, 12);
    memcpy(eth + 12, f + 20, 2);
    memcpy(eth + 14, f + 22, tl - 8);
    if (s->mode != TGT_FW || s->assoc != ASSOC_UP) {
        s->tx_drop++;
        return;
    }
    if (memcmp(eth + 6, s->sta_mac, 6)) {
        WLOG(s, 1, "tx: source %s is not the station address %s: dropped", mac_str(eth + 6),
             mac_str(s->sta_mac));
        s->tx_drop++;
        return;
    }
    uint16_t et = rd16be(eth + 12);
    if (et == 0x888E) {
        if (memcmp(eth, s->bssid, 6)) {
            WLOG(s, 1, "tx: EAPOL to %s instead of the AP: dropped", mac_str(eth));
        } else if (s->wpa2 && s->hs != HS_DONE) {           /* 4-way, or a group rekey */
            ap_rx_eapol(s, eth + 14, elen - 14);
        }
        return;
    }
    if (s->wpa2 && (!s->port_open || !s->sta_tk_ok)) {
        s->tx_drop++;
        if (s->tx_drop < 4 || !(s->tx_drop % 100)) {
            WLOG(s, 1, "tx: frame dropped by the AP: %s", !s->port_open ? "802.1X port closed" :
                 s->sta_tk_set ? "pairwise key differs (cannot decrypt)" : "no pairwise key installed");
        }
        return;
    }
    if (!memcmp(eth, s->bssid, 6)) {
        return;                                     /* addressed to the AP itself */
    }
    s->tx_frames++;
    if (s->tx_frames == 1) {
        WLOG(s, 1, "data: first frame from the station (ethertype %04x, %d bytes) -> uplink%s", et, elen,
             s->nic && qemu_get_queue(s->nic)->peer ? "" : " (none configured: -nic user,model=ctr-wifi)");
    }
    if (s->nic) {
        qemu_send_packet(qemu_get_queue(s->nic), eth, elen);
    }
}

static bool ap_can_receive(NetClientState *nc)
{
    return true;                                    /* full firmware buffers drop, like the air */
}

static ssize_t ap_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    CtrWifiState *s = qemu_get_nic_opaque(nc);
    if (size < 14 || size > 1514) {
        return size;
    }
    if (!(buf[0] & 1) && memcmp(buf, s->sta_mac, 6)) {
        return size;
    }
    if (deliver_to_sta(s, buf, size, false) && s->rx_frames == 1) {
        WLOG(s, 1, "data: first uplink frame delivered to the station (%zu bytes)", size);
    }
    return size;
}

static NetClientInfo ctr_wifi_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ap_can_receive,
    .receive = ap_receive,
};

/* ======================================================================== */
/* WMI commands (the firmware's side)                                        */
/* ======================================================================== */

static void cmd_error(CtrWifiState *s, uint16_t id, uint8_t code)
{
    uint8_t e[3];
    wr16le(e, id);
    e[2] = code;
    wmi_event(s, EV_CMD_ERROR, e, 3);
}

static bool len_ok(CtrWifiState *s, uint16_t id, uint32_t n, uint32_t lo, uint32_t hi)
{
    if (n >= lo && n <= hi) {
        return true;
    }
    WLOG(s, 1, "WMI command %04x with %u parameter bytes (expected %u..%u): INVALID_PARAM", id, n, lo, hi);
    cmd_error(s, id, 1);
    return false;
}

static void wmi_cmd(CtrWifiState *s, const uint8_t *b, uint32_t len)
{
    if (len < 2) {
        return;
    }
    uint16_t id = rd16le(b);
    const uint8_t *p = b + 2;
    uint32_t n = len - 2;
    WLOG(s, 2, "WMI command %04x, %u bytes", id, n);
    switch (id) {
    case WMI_CONNECT:
        if (!len_ok(s, id, n, 52, 52)) {
            return;
        }
        if (s->assoc != ASSOC_NONE) {
            WLOG(s, 1, "WMI_CONNECT while connected or connecting: ILLEGAL_STATE");
            cmd_error(s, id, 2);
            return;
        }
        memcpy(s->conn, p, 52);
        s->assoc = ASSOC_PENDING;
        timer_mod(s->assoc_timer, now_ns() + 20 * SCALE_MS);
        WLOG(s, 1, "WMI_CONNECT \"%.*s\" auth %u pairwise %u group %u, %u MHz, BSSID %s",
             p[7] > 32 ? 32 : p[7], (const char *)p + 8, p[2], p[3], p[5], rd16le(p + 40), mac_str(p + 42));
        break;
    case WMI_DISCONNECT:
        if (!len_ok(s, id, n, 0, 4)) {
            return;
        }
        timer_del(s->assoc_timer);
        if (s->assoc != ASSOC_NONE) {
            disconnect_event(s, DISC_CMD, 0);
        }
        break;
    case WMI_START_SCAN: {
        if (!len_ok(s, id, n, 18, 18 + 2 * 32)) {
            return;
        }
        int nch = p[17];
        if (n < 18u + 2 * nch) {
            cmd_error(s, id, 1);
            return;
        }
        s->scan_ch_mask = 0;
        for (int i = 0; i < nch; i++) {
            int c = channel_of(rd16le(p + 18 + 2 * i));
            if (c) {
                s->scan_ch_mask |= 1u << c;
            }
        }
        if (!nch) {
            s->scan_ch_mask = 0x7FFE;              /* channels 1-14 */
        }
        s->scanning = true;
        timer_mod(s->scan_timer, now_ns() + SCAN_CH_NS);
        WLOG(s, 1, "WMI_START_SCAN: %d channel(s)%s, BSS filter %u, probed SSID \"%s\"",
             nch ? nch : 14, nch ? "" : " (all)", s->bss_filter, (const char *)s->probed_ssid);
        break;
    }
    case WMI_SET_BSS_FILTER:
        if (len_ok(s, id, n, 1, 8)) {
            s->bss_filter = p[0];
        }
        break;
    case WMI_SET_PROBED_SSID:
        if (len_ok(s, id, n, 35, 35) && p[0] == 0) {
            s->probed_len = (p[1] && p[2] <= 32) ? p[2] : 0;
            memset(s->probed_ssid, 0, sizeof(s->probed_ssid));
            memcpy(s->probed_ssid, p + 3, s->probed_len);
        }
        break;
    case WMI_GET_CHANNEL_LIST: {
        uint8_t r[2 + 2 * 11];
        r[0] = 0;
        r[1] = 11;
        for (int i = 0; i < 11; i++) {
            wr16le(r + 2 + 2 * i, 2412 + 5 * i);
        }
        wmi_event(s, WMI_GET_CHANNEL_LIST, r, sizeof(r));
        break;
    }
    case WMI_SET_CHANNEL_PARAMS:
        if (len_ok(s, id, n, 4, 4 + 2 * 32) && n < 4u + 2 * p[3]) {
            cmd_error(s, id, 1);
        }
        break;
    case WMI_ADD_CIPHER_KEY: {
        if (!len_ok(s, id, n, 45, 51)) {
            return;
        }
        int idx = p[0], type = p[1], usage = p[2], klen = p[3];
        const uint8_t *key = p + 12;
        if (klen > 32) {
            cmd_error(s, id, 1);
            return;
        }
        if (usage == 0) {                           /* PAIRWISE_USAGE */
            s->sta_tk_set = true;
            s->sta_tk_ok = s->wpa2 && s->hs == HS_DONE &&
                           (s->wpa1 ? type == CRYPT_TKIP && klen == 32 && !memcmp(key, s->ptk + 32, 32) :
                                      type == CRYPT_AES && klen == 16 && !memcmp(key, s->ptk + 32, 16));
            WLOG(s, 1, "WMI_ADD_CIPHER_KEY pairwise, index %d, type %d, %d bytes: %s", idx, type, klen,
                 s->sta_tk_ok ? "equals the AP's TK" : "DOES NOT equal the AP's TK");
        } else if (usage == 1) {                    /* GROUP_USAGE */
            uint8_t want[32];
            int wl = s->gtk_len;
            memcpy(want, s->gtk, wl);
            if (wl == 32) {                         /* firmware order: MIC halves swapped */
                memcpy(want + 16, s->gtk + 24, 8);
                memcpy(want + 24, s->gtk + 16, 8);
            }
            s->sta_gtk_set = true;
            s->sta_gtk_ok = s->wpa2 && s->hs == HS_DONE && idx == s->gtk_idx && klen == wl &&
                            type == (wl == 32 ? CRYPT_TKIP : CRYPT_AES) && !memcmp(key, want, wl);
            WLOG(s, 1, "WMI_ADD_CIPHER_KEY group, index %d, type %d, %d bytes: %s", idx, type, klen,
                 s->sta_gtk_ok ? "equals the AP's GTK" : "DOES NOT equal the AP's GTK");
        } else {
            WLOG(s, 1, "WMI_ADD_CIPHER_KEY usage %d ignored", usage);
        }
        break;
    }
    case WMI_SYNCHRONIZE: case WMI_SET_POWER_MODE: case WMI_SET_DISC_TIMEOUT:
    case WMI_SET_KEEPALIVE: case WMI_SET_WSC_STATUS: case WMI_SET_TX_PWR:
    case WMI_DELETE_BAD_AP: case WMI_DELETE_CIPHER_KEY:
        len_ok(s, id, n, 1, 1);
        break;
    case WMI_TARGET_ERROR_BITMASK:
        len_ok(s, id, n, 4, 4);
        break;
    case WMI_SET_HB_TIMEOUT:
        len_ok(s, id, n, 1, 4);
        break;
    case WMI_SET_SCAN_PARAMS:
        len_ok(s, id, n, 16, 24);
        break;
    case WMI_CREATE_PSTREAM: case WMI_WMIX: case WMI_SET_FIXRATES: case WMI_SET_FRAMERATES:
    case WMI_NINTENDO_BITRATE: case WMI_SET_LISTEN_INT: case WMI_RECONNECT:
        break;
    default:
        WLOG(s, 1, "unknown WMI command %04x (%u bytes): INVALID_PARAM", id, n);
        cmd_error(s, id, 1);
        break;
    }
}

/* ======================================================================== */
/* HTC (firmware): one message from the host                                 */
/* ======================================================================== */

static void htc_rx(CtrWifiState *s, const uint8_t *m, uint32_t avail)
{
    if (avail < 6) {
        WLOG(s, 1, "HTC: %u-byte message dropped", avail);
        return;
    }
    int ep = m[0];
    uint32_t len = rd16le(m + 2);
    if (len + 6 > avail) {
        WLOG(s, 1, "HTC: header says %u payload bytes, only %u arrived: dropped", len, avail - 6);
        return;
    }
    if (ep >= 8 || !s->ep_svc[ep]) {
        WLOG(s, 1, "HTC: message for unconnected endpoint %d dropped", ep);
        return;
    }
    /* ath6kl htc_mbox.c: every message consumes credits for the padded
       length. A host that ignores the reports overruns the firmware mailbox
       ("Mbox Full", dsiwifi): refuse the message and flag TX overflow. */
    uint32_t need = (ROUND_UP(avail, io_block(s)) + HTC_CREDIT_SIZE - 1) / HTC_CREDIT_SIZE;
    if (!need) {
        need = 1;
    }
    if (s->credits[ep] < need) {
        s->credit_overflow++;
        s->err_int |= ERR_TX_OVERFLOW;
        WLOG(s, 1, "HTC: Mbox Full on endpoint %d (%u credits needed, %u left): message dropped",
             ep, need, s->credits[ep]);
        return;
    }
    s->credits[ep] -= need;
    s->credit_used += need;
    s->owed[ep] = MIN(s->owed[ep] + need, 255);
    const uint8_t *p = m + 6;
    if (ep == 0) {
        uint16_t id = len >= 2 ? rd16le(p) : 0;
        if (id == 2 && len >= 8) {                  /* CONNECT_SERVICE */
            uint16_t svc = rd16le(p + 2);
            uint8_t r[10] = { 3, 0 };
            bool known = svc >= 0x0100 && svc <= 0x0104, dup = false;
            wr16le(r + 2, svc);
            for (int e = 0; e < 8; e++) {
                dup |= s->ep_svc[e] == svc;
            }
            if (!known || dup || s->next_ep >= 8) {
                r[4] = known ? 2 : 1;               /* FAILED / NOT_FOUND */
                r[5] = 0xFF;
            } else {
                r[4] = 0;
                r[5] = s->next_ep;
                s->ep_svc[s->next_ep] = svc;
                /* one credit for one maximum-sized message, like ath6kl's
                   htc_setup_tx_allocs; the remainder goes to the BE data ep */
                if (s->credit_pool) {
                    s->credits[s->next_ep] = 1;
                    s->credit_pool--;
                }
                if (svc == 0x0100) {
                    s->wmi_ep = s->next_ep;
                }
                s->next_ep++;
            }
            wr16le(r + 6, HTC_CREDIT_SIZE);
            htc_queue(s, 0, r, sizeof(r));
            WLOG(s, 1, "HTC: service %04x -> endpoint %d%s", svc, r[4] ? -1 : r[5],
                 r[4] ? " (refused)" : "");
        } else if ((id == 4 || id == 5) && len >= 2) { /* SETUP_COMPLETE(_EX) */
            if (s->wmi_ep < 0) {
                WLOG(s, 1, "HTC: setup complete without a WMI control service: firmware idles");
                return;
            }
            s->htc_setup_done = true;
            /* the host and the target must agree: the leftover credits all go
               to the first data endpoint (ath6kl's BE endpoint) */
            for (int e = 1; e < 8 && s->credit_pool; e++) {
                if (s->ep_svc[e] && e != s->wmi_ep) {
                    s->credits[e] += s->credit_pool;
                    s->credit_pool = 0;
                    WLOG(s, 1, "HTC: endpoint %d has %u credits (%u bytes each)",
                         e, s->credits[e], HTC_CREDIT_SIZE);
                }
            }
            uint8_t rd[7], reg[4] = { 0 };
            memcpy(rd, s->sta_mac, 6);
            rd[6] = 0x02;                           /* phy capability: 11g */
            wmi_event(s, EV_READY, rd, 7);
            wmi_event(s, EV_REG_DOMAIN, reg, 4);
            WLOG(s, 1, "HTC setup complete: WMI_READY, MAC %s", mac_str(s->sta_mac));
        } else {
            WLOG(s, 1, "HTC: control message %04x (%u bytes) ignored", id, len);
        }
    } else if (ep == s->wmi_ep) {
        if (!s->htc_setup_done) {
            WLOG(s, 1, "WMI command before HTC setup complete: dropped");
            return;
        }
        wmi_cmd(s, p, len);
    } else {
        sta_tx(s, p, len);
    }
    if (m[1] & HTC_FLG_NEED_CREDIT) {
        s->credit_waits++;
        timer_mod(s->credit_timer, now_ns() + SCALE_MS);
    }
}

/* ======================================================================== */
/* function 1 / mailbox                                                      */
/* ======================================================================== */

/* mailbox 0 windows and their end-of-message address: 0x000-0x0FF (DSi),
   0x800-0xFFF (ath6kl HIF_MBOX_BASE_ADDR) and the extended window up to
   0x3FFF that dsiwifi uses for block transfers (0x4000 - length) */
static int mbox0_end(uint32_t a)
{
    if (a <= 0x0FF) {
        return 0x0FF;
    }
    if (a >= 0x800 && a <= 0xFFF) {
        return 0xFFF;
    }
    if (a >= 0x1000 && a <= 0x3FFF) {
        return 0x3FFF;
    }
    return -1;
}

static uint8_t mbox_pop(CtrWifiState *s)
{
    if (!s->out->len) {
        s->err_int |= ERR_RX_UNDERFLOW;
        GERR("mailbox read with nothing pending (RX underflow)");
        return 0;
    }
    uint8_t v = s->out->data[0];
    g_byte_array_remove_index(s->out, 0);
    return v;
}

static void mbox_push(CtrWifiState *s, uint32_t addr, uint8_t v, int end)
{
    if (s->mode != TGT_BMI && s->mode != TGT_FW) {
        return;                                     /* nobody listens */
    }
    if (s->in_len >= sizeof(s->in)) {
        s->err_int |= ERR_TX_OVERFLOW;
        return;
    }
    s->in[s->in_len++] = v;
    if (s->mode == TGT_BMI) {
        bmi_feed(s);
    } else if (addr == (uint32_t)end) {             /* end of message */
        htc_rx(s, s->in, s->in_len);
        s->in_len = 0;
    }
}

static uint8_t f1_read(CtrWifiState *s, uint32_t a)
{
    if (mbox0_end(a) >= 0) {
        return mbox_pop(s);
    }
    if (a < 0x400 || a > 0x4FF) {
        GERR("function 1 read of unmodelled address %05x", a);
        return 0;
    }
    switch (a) {
    case F1_HOST_INT_STATUS:
        return (s->out->len ? 1 : 0) | (s->err_int ? 0x80 : 0);
    case F1_ERROR_INT_STATUS:
        return s->err_int;
    case F1_CPU_INT_STATUS: case F1_COUNTER_INT_STATUS:
        return 0;
    case F1_MBOX_FRAME:
        return s->out->len ? 1 : 0;
    case F1_RX_LOOKAHEAD_VALID:
        return s->out->len >= 4 ? 1 : 0;
    case F1_RX_LOOKAHEAD0 ... F1_RX_LOOKAHEAD0 + 3:
        return s->out->len >= 4 ? s->out->data[a - F1_RX_LOOKAHEAD0] : 0;
    case F1_COUNT + 16:                             /* credit counter 4: BMI */
        return s->bmi_credits;
    case F1_COUNT_DEC + 16: {                       /* reading takes a credit */
        uint8_t v = s->bmi_credits;
        if (v) {
            s->bmi_credits--;
        }
        return v;
    }
    }
    return s->f1regs[a - 0x400];
}

static void f1_write(CtrWifiState *s, uint32_t a, uint8_t v)
{
    int end = mbox0_end(a);
    if (end >= 0) {
        mbox_push(s, a, v, end);
        return;
    }
    if (a < 0x400 || a > 0x4FF) {
        GERR("function 1 write of unmodelled address %05x", a);
        return;
    }
    if (a == F1_ERROR_INT_STATUS) {
        s->err_int &= ~v;
        return;
    }
    s->f1regs[a - 0x400] = v;
    if (a == F1_WINDOW_WRITE) {                     /* LSB written last starts the cycle */
        uint32_t addr = rd32le(s->f1regs + (F1_WINDOW_WRITE - 0x400));
        uint32_t data = rd32le(s->f1regs + (F1_WINDOW_DATA - 0x400));
        WLOG(s, 2, "window write [%08x] = %08x", addr, data);
        target_write32(s, addr, data);
    } else if (a == F1_WINDOW_READ) {
        uint32_t addr = rd32le(s->f1regs + (F1_WINDOW_READ - 0x400));
        wr32le(s->f1regs + (F1_WINDOW_DATA - 0x400), target_read32(s, addr));
    }
}

/* ======================================================================== */
/* SDIO card                                                                 */
/* ======================================================================== */

#define CARD_OCR 0x00FF8000u

static const uint8_t cis[] = {
    0x01, 0x03, 0xD9, 0x01, 0xFF,                   /* CISTPL_DEVICE */
    0x20, 0x04, 0x71, 0x02, 0x01, 0x02,             /* CISTPL_MANFID: Atheros 0271, card 0201 */
    0x21, 0x02, 0x0C, 0x00,                         /* CISTPL_FUNCID: SDIO */
    0xFF,
};

static uint8_t f0_read(CtrWifiState *s, uint32_t a)
{
    switch (a) {
    case 0x00: return 0x22;                         /* CCCR / SDIO revision */
    case 0x01: return 0x02;
    case 0x02: return s->ioe;
    case 0x03: return s->ioe & 0x02;                /* function 1 ready once enabled */
    case 0x04: return s->ien;
    case 0x07: return s->busif;
    case 0x08: return 0x12;                         /* multi-block, 4-bit interrupts */
    case 0x0A: case 0x10A: return 0x10;             /* CIS at 0x001000 */
    case 0x10: return s->fn0_blksz;
    case 0x11: return s->fn0_blksz >> 8;
    case 0x12: return s->pwrctl | 1;
    case 0x13: return 0x01;
    case 0x100: return 0x07;                        /* standard function: WLAN */
    case 0x110: return s->fn1_blksz;
    case 0x111: return s->fn1_blksz >> 8;
    }
    if (a >= 0x1000 && a < 0x1000 + sizeof(cis)) {
        return cis[a - 0x1000];
    }
    return 0;
}

static void f0_write(CtrWifiState *s, uint32_t a, uint8_t v)
{
    switch (a) {
    case 0x02: s->ioe = v & 0x02; break;
    case 0x04: s->ien = v & 0x03; break;
    case 0x06:
        if (v & 0x08) {                             /* RES: reset the I/O functions */
            s->ioe = 0;
            s->cx.active = false;
        }
        break;
    case 0x07: s->busif = v & 0x83; break;
    case 0x10: s->fn0_blksz = (s->fn0_blksz & 0xFF00) | v; break;
    case 0x11: s->fn0_blksz = (s->fn0_blksz & 0x00FF) | v << 8; break;
    case 0x12: s->pwrctl = v & 0x02; break;
    case 0x110: s->fn1_blksz = (s->fn1_blksz & 0xFF00) | v; break;
    case 0x111: s->fn1_blksz = (s->fn1_blksz & 0x00FF) | v << 8; break;
    }
}

static uint8_t card_rd(CtrWifiState *s, uint32_t func, uint32_t a, bool *err)
{
    if (func == 0) {
        return f0_read(s, a);
    }
    if (func == 1 && (s->ioe & 2)) {
        return f1_read(s, a);
    }
    *err = true;
    return 0;
}

static void card_wr(CtrWifiState *s, uint32_t func, uint32_t a, uint8_t v, bool *err)
{
    if (func == 0) {
        f0_write(s, a, v);
    } else if (func == 1 && (s->ioe & 2)) {
        f1_write(s, a, v);
    } else {
        *err = true;
    }
}

/* returns false when the card does not answer (the host sees a timeout) */
static bool card_command(CtrWifiState *s, int idx, uint32_t arg, uint32_t *resp)
{
    *resp = 0;
    switch (idx) {
    case 5:                                         /* IO_SEND_OP_COND, R4 */
        if (s->cstate >= CARD_STBY) {
            return false;
        }
        if (!(arg & 0xFFFFFF)) {                     /* inquiry: OCR, not ready */
            *resp = 1u << 28 | CARD_OCR;
            return true;
        }
        if (!(arg & CARD_OCR)) {
            return false;                           /* no common voltage: inactive */
        }
        s->cstate = CARD_READY;
        *resp = 1u << 31 | 1u << 28 | CARD_OCR;
        return true;
    case 3:                                         /* SEND_RELATIVE_ADDR, R6 */
        if (s->cstate != CARD_READY && s->cstate != CARD_STBY) {
            return false;
        }
        s->rca = 0x0001;
        s->cstate = CARD_STBY;
        *resp = (uint32_t)s->rca << 16;
        return true;
    case 7:                                         /* SELECT_CARD, R1b */
        if (s->cstate < CARD_STBY) {
            return false;
        }
        if ((arg >> 16) != s->rca) {
            s->cstate = CARD_STBY;
            return false;
        }
        s->cstate = CARD_CMD;
        *resp = 0x1E00;
        return true;
    case 52: {                                      /* IO_RW_DIRECT, R5 */
        if (s->cstate != CARD_CMD) {
            return false;
        }
        bool w = arg >> 31, raw = (arg >> 27) & 1, err = false;
        uint32_t func = (arg >> 28) & 7, a = (arg >> 9) & 0x1FFFF;
        uint8_t r = 0;
        if (w) {
            card_wr(s, func, a, arg & 0xFF, &err);
            if (raw) {
                r = card_rd(s, func, a, &err);
            }
        } else {
            r = card_rd(s, func, a, &err);
        }
        if (err) {
            GERR("CMD52 to disabled/absent function %u (address %05x)", func, a);
        }
        *resp = (err ? 0x12 : 0x10) << 8 | r;
        return true;
    }
    case 53: {                                      /* IO_RW_EXTENDED, R5 */
        if (s->cstate != CARD_CMD) {
            return false;
        }
        uint32_t func = (arg >> 28) & 7, cnt = arg & 0x1FF;
        bool block = (arg >> 27) & 1;
        if (func != 1 || !(s->ioe & 2)) {
            GERR("CMD53 to function %u (not enabled)", func);
            s->cx.active = false;
            *resp = 0x12 << 8;
            return true;
        }
        s->cx.active = true;
        s->cx.write = arg >> 31;
        s->cx.inc = (arg >> 26) & 1;
        s->cx.func = func;
        s->cx.addr = (arg >> 9) & 0x1FFFF;
        s->cx.bytes = block ? cnt * s->fn1_blksz : (cnt ? cnt : 512);
        if (block && (!cnt || !s->fn1_blksz)) {
            GERR("CMD53 block mode with count %u, block size %u", cnt, s->fn1_blksz);
            s->cx.active = false;
        }
        *resp = 0x20 << 8;
        return true;
    }
    default:                                        /* CMD0 etc.: no response */
        return false;
    }
}

static void card_xfer_byte(CtrWifiState *s, uint8_t *v)
{
    bool err = false;
    if (s->cx.write) {
        card_wr(s, s->cx.func, s->cx.addr, *v, &err);
    } else {
        *v = card_rd(s, s->cx.func, s->cx.addr, &err);
    }
    if (s->cx.inc) {
        s->cx.addr++;
    }
    s->cx.bytes--;
}

/* ======================================================================== */
/* TMIO controller 2                                                         */
/* ======================================================================== */

static bool card_reachable(CtrWifiState *s)
{
    return s->powered && (s->regs[T_PORTSEL / 2] & 3) == 0 && (s->regs[T_CLKCTL / 2] & 0x100);
}

static void tmio_end_data(CtrWifiState *s)
{
    s->xfer_rd = s->xfer_wr = s->rx_full = false;
    s->stat0 |= ST0_DATAEND;
    s->stat1 &= ~(ST1_RXRDY | ST1_TXRQ);
    if (!s->cx.bytes) {
        s->cx.active = false;
    }
}

static void tmio_load_block(CtrWifiState *s)
{
    if (!s->blocks_left) {
        tmio_end_data(s);
        return;
    }
    for (uint32_t i = 0; i < s->blklen; i++) {
        uint8_t v = 0;
        if (s->cx.active && s->cx.bytes) {
            card_xfer_byte(s, &v);
        }
        s->block[i] = v;
    }
    s->pos = 0;
    s->rx_full = true;
    s->stat1 |= ST1_RXRDY;
}

static void tmio_command(CtrWifiState *s, uint16_t cmdreg)
{
    int idx = cmdreg & 0x3F;
    unsigned rtype = (cmdreg >> 8) & 7;
    bool data = cmdreg & 0x0800, read = cmdreg & 0x1000;
    uint32_t arg = s->regs[T_ARG0 / 2] | (uint32_t)s->regs[T_ARG1 / 2] << 16, r = 0;
    bool answered = false;

    s->xfer_rd = s->xfer_wr = s->rx_full = false;
    if (card_reachable(s)) {
        answered = card_command(s, idx, arg, &r);
    }
    s->resp[0] = r;
    s->resp[1] = r >> 16;
    s->stat0 |= ST0_RESPEND;
    if (rtype != 3 && !answered) {
        s->stat1 |= ST1_CMDTMO;
        WLOG(s, 3, "CMD%d arg %08x: no response", idx, arg);
        return;
    }
    if (!data) {
        return;
    }
    uint32_t nblk = (cmdreg & 0x2000) ? s->regs[T_BLKCNT / 2] : 1;
    bool d32 = s->regs[T_DATA32CTL / 2] & D32_EN;
    s->blklen = (d32 && s->regs[T_BLKLEN32 / 2]) ? s->regs[T_BLKLEN32 / 2] : s->regs[T_BLKLEN / 2];
    if (!s->blklen || s->blklen > 512) {
        s->blklen = 512;
    }
    if (!s->cx.active || s->cx.write == read || s->cx.bytes != nblk * s->blklen) {
        GERR("CMD%d data phase: card expects %u bytes (%s), controller set up %u x %u",
             idx, s->cx.active ? s->cx.bytes : 0, s->cx.write ? "write" : "read", nblk, s->blklen);
        WLOG(s, 1, "SDIO: CMD%d data phase mismatch (card %u bytes, controller %u x %u): data timeout",
             idx, s->cx.active ? s->cx.bytes : 0, nblk, s->blklen);
        s->stat1 |= ST1_DATATMO;
        s->cx.active = false;
        return;
    }
    bool card4 = (s->busif & 3) == 2, host4 = !(s->regs[T_OPT / 2] & 0x8000);
    if (card4 != host4) {
        WLOG(s, 1, "SDIO: bus width: card %d-bit, controller %d-bit: data CRC error",
             card4 ? 4 : 1, host4 ? 4 : 1);
        s->stat1 |= ST1_CRCFAIL;
        s->cx.active = false;
        return;
    }
    s->blocks_left = nblk;
    s->pos = 0;
    if (read) {
        s->xfer_rd = true;
        tmio_load_block(s);
    } else {
        s->xfer_wr = true;
        if (s->fifo32) {
            s->stat1 |= ST1_TXRQ;
        }
    }
}

static uint32_t tmio_fifo_read(CtrWifiState *s, unsigned size)
{
    uint32_t v = 0;
    if (!s->xfer_rd || !s->rx_full) {
        GERR("TMIO2 FIFO read with no data");
        return 0;
    }
    for (unsigned i = 0; i < size && s->pos < s->blklen; i++) {
        v |= (uint32_t)s->block[s->pos++] << (8 * i);
    }
    if (s->pos >= s->blklen) {
        s->rx_full = false;
        s->blocks_left--;
        tmio_load_block(s);
    }
    return v;
}

static void tmio_fifo_write(CtrWifiState *s, uint32_t v, unsigned size)
{
    if (!s->xfer_wr) {
        GERR("TMIO2 FIFO write with no transfer");
        return;
    }
    for (unsigned i = 0; i < size && s->pos < s->blklen; i++) {
        s->block[s->pos++] = v >> (8 * i);
    }
    if (s->pos >= s->blklen) {
        for (uint32_t i = 0; i < s->blklen && s->cx.active && s->cx.bytes; i++) {
            card_xfer_byte(s, &s->block[i]);
        }
        s->pos = 0;
        if (--s->blocks_left == 0) {
            tmio_end_data(s);
        } else {
            s->stat1 |= ST1_TXRQ;
        }
    }
}

static uint16_t tmio_rd16(CtrWifiState *s, hwaddr a)
{
    switch (a) {
    case T_STAT0:
        return s->stat0;
    case T_STAT1:
        return s->stat1 | ST1_NOTBUSY | ST1_CLKDIVOK;
    case T_RESP0:
        return s->resp[0];
    case T_RESP0 + 2:
        return s->resp[1];
    case T_RESP0 + 4 ... T_RESP0 + 14:
        return 0;
    case T_DATA32CTL: {
        uint16_t v = s->regs[a / 2] & D32_RW;
        if (s->rx_full && (v & D32_EN) && s->fifo32) {
            v |= D32_FULL;
        }
        return v;
    }
    }
    return a < sizeof(s->regs) ? s->regs[a / 2] : 0;
}

static void tmio_wr16(CtrWifiState *s, hwaddr a, uint16_t v)
{
    switch (a) {
    case T_CMD:
        s->regs[0] = v;
        tmio_command(s, v);
        return;
    case T_STAT0:
        s->stat0 &= v;
        return;
    case T_STAT1:
        s->stat1 &= v;
        return;
    case T_DATA32CTL:
        v &= D32_RW;
        break;
    case T_ERRSTAT0: case T_ERRSTAT0 + 2:
        return;
    case T_RESET:
        if (!(v & 1)) {
            s->stat0 = s->stat1 = 0;
            s->xfer_rd = s->xfer_wr = s->rx_full = false;
        }
        break;
    }
    if (a < sizeof(s->regs)) {
        s->regs[a / 2] = v;
    }
}

static uint64_t tmio_read(void *opaque, hwaddr a, unsigned size)
{
    CtrWifiState *s = opaque;
    if (a == T_FIFO32) {
        if (!(s->regs[T_DATA32CTL / 2] & D32_EN) || !s->fifo32) {
            GERR("TMIO2 FIFO32 read while disabled");
            return 0;
        }
        return tmio_fifo_read(s, size);
    }
    if (a == T_FIFO16) {
        return tmio_fifo_read(s, size);
    }
    if (size == 4) {
        return tmio_rd16(s, a) | (uint32_t)tmio_rd16(s, a + 2) << 16;
    }
    uint16_t v = tmio_rd16(s, a & ~1);
    return size == 1 ? (a & 1 ? v >> 8 : v & 0xFF) : v;
}

static void tmio_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    CtrWifiState *s = opaque;
    if (a == T_FIFO32 || a == T_FIFO16) {
        if (a == T_FIFO32 && (!(s->regs[T_DATA32CTL / 2] & D32_EN) || !s->fifo32)) {
            GERR("TMIO2 FIFO32 write while disabled");
            return;
        }
        tmio_fifo_write(s, v, size);
        return;
    }
    if (size == 4) {
        tmio_wr16(s, a, v);
        tmio_wr16(s, a + 2, v >> 16);
    } else if (size == 2) {
        tmio_wr16(s, a, v);
    } else {
        uint16_t cur = (a & ~1) == T_STAT0 || (a & ~1) == T_STAT1 ? 0xFFFF : tmio_rd16(s, a & ~1);
        cur = (a & 1) ? ((cur & 0x00FF) | (v << 8)) : ((cur & 0xFF00) | (v & 0xFF));
        tmio_wr16(s, a & ~1, cur);
    }
}

static const MemoryRegionOps tmio_ops = {
    .read = tmio_read, .write = tmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ======================================================================== */
/* power: CFG11_WIFICNT and the two GPIOs                                    */
/* ======================================================================== */

static void card_reset(CtrWifiState *s)
{
    s->cstate = CARD_IDLE;
    s->rca = 0;
    s->ioe = s->ien = s->busif = s->pwrctl = 0;
    s->fn0_blksz = s->fn1_blksz = 0;
    s->cx.active = false;
    memset(s->f1regs, 0, sizeof(s->f1regs));
}

static void target_reset(CtrWifiState *s, bool cold)
{
    if (cold) {
        memset(s->ram, 0, AR_RAM_SIZE);
        memset(s->written, 0, AR_RAM_SIZE >> 8);
        s->nsocregs = 0;
    }
    *socreg(s, SOC_RESET_CAUSE, true) = cold ? 0 : 2;
    s->mode = s->powered ? TGT_BMI : TGT_OFF;
    s->in_len = 0;
    g_byte_array_set_size(s->out, 0);
    s->bmi_credits = 1;
    s->err_int = 0;
    s->lz_active = s->stub_ran = false;
    s->lz_bytes = s->wr_bytes = 0;
    s->htc_setup_done = false;
    memset(s->ep_svc, 0, sizeof(s->ep_svc));
    memset(s->owed, 0, sizeof(s->owed));
    memset(s->credits, 0, sizeof(s->credits));
    s->credits[0] = 1;                      /* endpoint 0 always gets one */
    s->credit_pool = HTC_CREDITS - 1;       /* the rest are handed out on CONNECT_SERVICE */
    s->credit_used = s->credit_reports = s->credit_overflow = s->credit_waits = 0;
    s->wmi_ep = -1;
    s->bss_filter = 0;
    s->probed_len = 0;
    memset(s->probed_ssid, 0, sizeof(s->probed_ssid));
    s->scanning = false;
    s->assoc = ASSOC_NONE;
    s->hs = HS_IDLE;
    s->port_open = false;
    s->sta_tk_set = s->sta_gtk_set = s->sta_tk_ok = s->sta_gtk_ok = false;
    timer_del(s->boot_timer);
    timer_del(s->scan_timer);
    timer_del(s->assoc_timer);
    timer_del(s->hs_timer);
    timer_del(s->credit_timer);
}

static void power_update(CtrWifiState *s)
{
    bool on = (s->wificnt & 1) && (s->gpio[0x28 / 2] & 1) && !(s->gpio[0x14 / 2] & 1);
    if (on == s->powered) {
        return;
    }
    s->powered = on;
    card_reset(s);
    target_reset(s, true);
    WLOG(s, 1, "card %s (WIFICNT %u, GPIO2_2 %u, GPIO3_12/nRESET %u)", on ? "powered and out of reset" :
         "off or in reset", s->wificnt & 1, s->gpio[0x14 / 2] & 1, s->gpio[0x28 / 2] & 1);
}

static uint64_t gpio_read(void *opaque, hwaddr a, unsigned size)
{
    CtrWifiState *s = opaque;
    uint32_t v = s->gpio[a / 2];
    if (size == 4 && a / 2 + 1 < ARRAY_SIZE(s->gpio)) {
        v |= (uint32_t)s->gpio[a / 2 + 1] << 16;
    }
    if (size == 1) {
        return (a & 1) ? v >> 8 & 0xFF : v & 0xFF;
    }
    return v;
}

static void gpio_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    CtrWifiState *s = opaque;
    if (size == 1) {
        uint16_t cur = s->gpio[a / 2];
        s->gpio[a / 2] = (a & 1) ? ((cur & 0xFF) | v << 8) : ((cur & 0xFF00) | (v & 0xFF));
    } else {
        s->gpio[a / 2] = v;
        if (size == 4 && a / 2 + 1 < ARRAY_SIZE(s->gpio)) {
            s->gpio[a / 2 + 1] = v >> 16;
        }
    }
    power_update(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t wificnt_read(void *opaque, hwaddr a, unsigned size)
{
    CtrWifiState *s = opaque;
    return a == 0 ? s->wificnt : 0;
}

static void wificnt_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    CtrWifiState *s = opaque;
    if (a == 0) {
        s->wificnt = v & 1;
        power_update(s);
    }
}

static const MemoryRegionOps wificnt_ops = {
    .read = wificnt_read, .write = wificnt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ======================================================================== */
/* device                                                                    */
/* ======================================================================== */

static void ctr_wifi_reset(DeviceState *dev)
{
    CtrWifiState *s = CTR_WIFI(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->stat0 = s->stat1 = 0;
    s->xfer_rd = s->xfer_wr = s->rx_full = false;
    s->wificnt = 0;
    memset(s->gpio, 0, sizeof(s->gpio));
    s->powered = false;
    card_reset(s);
    target_reset(s, true);
}

static void ctr_wifi_realize(DeviceState *dev, Error **errp)
{
    CtrWifiState *s = CTR_WIFI(dev);
    static const uint8_t bssid[6] = AP_BSSID;

    if (!s->ssid || !s->ssid[0] || strlen(s->ssid) > 32) {
        error_setg(errp, "ctr-wifi: ssid must be 1-32 bytes");
        return;
    }
    s->wpa2 = !s->security || strcmp(s->security, "open");
    s->sec_mode = !s->security ? SEC_WPA2 : !strcmp(s->security, "wpa2-wpa3") ? SEC_MIXED :
                  !strcmp(s->security, "wpa3") ? SEC_WPA3 :
                  !strcmp(s->security, "wpa1") ? SEC_WPA1 : SEC_WPA2;
    s->wpa1 = s->sec_mode == SEC_WPA1;
    if (s->wpa2 && (!s->psk || strlen(s->psk) < 8 || strlen(s->psk) > 63)) {
        error_setg(errp, "ctr-wifi: psk must be an 8-63 character passphrase");
        return;
    }
    s->group_tkip = s->group && !strcmp(s->group, "tkip");
    s->channel_mhz = (s->channel_mhz >= 1 && s->channel_mhz <= 13) ? 2407 + 5 * s->channel_mhz : 2437;
    memcpy(s->bssid, bssid, 6);
    if (s->wpa2) {
        psk_to_pmk(s->psk, s->ssid, s->pmk);
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    memcpy(s->sta_mac, s->conf.macaddr.a, 6);
    s->nic = qemu_new_nic(&ctr_wifi_net_info, &s->conf, object_get_typename(OBJECT(dev)),
                          dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->ram = g_malloc0(AR_RAM_SIZE);
    s->written = g_malloc0(AR_RAM_SIZE >> 8);
    s->out = g_byte_array_new();
    s->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fw_boot, s);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan_step, s);
    s->assoc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, assoc_done, s);
    s->hs_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hs_timeout, s);
    s->rekey_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rekey_timeout, s);
    s->credit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, credit_flush, s);

    memory_region_init_io(&s->tmio_mr, OBJECT(dev), &tmio_ops, s, "ctr.tmio2-wifi", 0x200);
    memory_region_init_io(&s->gpio_mr, OBJECT(dev), &gpio_ops, s, "ctr.gpio", 0x30);
    memory_region_init_io(&s->wificnt_mr, OBJECT(dev), &wificnt_ops, s, "ctr.cfg11-wificnt", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->tmio_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->gpio_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->wificnt_mr);
    WLOG(s, 2, "AP \"%s\" %s on %u MHz, %s%s; station MAC %s", s->ssid, mac_str(s->bssid),
         s->channel_mhz, s->wpa2 ? "WPA2-PSK/CCMP" : "open", s->group_tkip ? " (TKIP group)" : "",
         mac_str(s->sta_mac));
}

static const Property ctr_wifi_props[] = {
    DEFINE_NIC_PROPERTIES(CtrWifiState, conf),
    DEFINE_PROP_STRING("ssid", CtrWifiState, ssid),
    DEFINE_PROP_STRING("psk", CtrWifiState, psk),
    DEFINE_PROP_STRING("security", CtrWifiState, security),
    DEFINE_PROP_STRING("group", CtrWifiState, group),
    DEFINE_PROP_UINT32("channel", CtrWifiState, channel_mhz, 6),
    DEFINE_PROP_BOOL("hidden", CtrWifiState, hidden, false),
    /* "fifo32=off": the hypothesis that the ARM9 cannot use TMIO2's FIFO at
       +0x10C (docs/wifi.md 7.2); the driver must fall back to CMD52 */
    DEFINE_PROP_BOOL("fifo32", CtrWifiState, fifo32, true),
    DEFINE_PROP_BOOL("decoys", CtrWifiState, decoys, true),
    DEFINE_PROP_UINT32("rekey", CtrWifiState, rekey_s, 0),
    /* HTC credits: read with HMP 'qom-get /machine/...' (diagnostics, docs/wifi.md 7) */
    DEFINE_PROP_UINT32("htc-credits-used", CtrWifiState, credit_used, 0),
    DEFINE_PROP_UINT32("htc-credit-reports", CtrWifiState, credit_reports, 0),
    DEFINE_PROP_UINT32("htc-credit-waits", CtrWifiState, credit_waits, 0),
    DEFINE_PROP_UINT32("htc-overflows", CtrWifiState, credit_overflow, 0),
    DEFINE_PROP_UINT32("wifi-tx-frames", CtrWifiState, tx_frames, 0),
    DEFINE_PROP_UINT32("wifi-rx-frames", CtrWifiState, rx_frames, 0),
    DEFINE_PROP_UINT32("wifi-tx-drop", CtrWifiState, tx_drop, 0),
    DEFINE_PROP_UINT32("wifi-rx-drop", CtrWifiState, rx_drop, 0),
    DEFINE_PROP_UINT32("debug", CtrWifiState, debug, 1),
};

static void ctr_wifi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ctr_wifi_realize;
    device_class_set_legacy_reset(dc, ctr_wifi_reset);
    device_class_set_props(dc, ctr_wifi_props);
    dc->user_creatable = false;
}

static const TypeInfo ctr_wifi_info = {
    .name = TYPE_CTR_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CtrWifiState),
    .class_init = ctr_wifi_class_init,
};

static void ctr_wifi_register_types(void)
{
    type_register_static(&ctr_wifi_info);
}
type_init(ctr_wifi_register_types)

static void default_str(DeviceState *dev, const char *prop, const char *val)
{
    g_autofree char *cur = object_property_get_str(OBJECT(dev), prop, NULL);
    if (!cur || !*cur) {
        qdev_prop_set_string(dev, prop, val);
    }
}

void ctr_wifi_init(MemoryRegion *io_shared, hwaddr io_shared_base)
{
    DeviceState *dev = qdev_new(TYPE_CTR_WIFI);
    /* -nic user,model=ctr-wifi (or a plain -nic user) becomes the AP's uplink */
    qemu_configure_nic_device(dev, true, NULL);
    default_str(dev, "ssid", "ios3ds-qemu");
    default_str(dev, "psk", "ios3ds-test-passphrase");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    SysBusDevice *sb = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(io_shared, 0x10122000 - io_shared_base, sysbus_mmio_get_region(sb, 0));
    memory_region_add_subregion(io_shared, 0x10147000 - io_shared_base, sysbus_mmio_get_region(sb, 1));
    /* CFG11_WIFICNT sits inside the board's CFG11 page: map it on top */
    memory_region_add_subregion_overlap(io_shared, 0x10140180 - io_shared_base,
                                        sysbus_mmio_get_region(sb, 2), 1);
}
