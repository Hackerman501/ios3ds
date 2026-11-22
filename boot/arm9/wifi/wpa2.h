/*
 * WPA2-PSK (RSN, CCMP) supplicant core for the ARM9 Wi-Fi driver.
 * Pure C, no allocation, unit-tested on the host against the IEEE 802.11i /
 * RFC 2202 / RFC 3394 / FIPS-197 vectors and a simulated authenticator.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "wifi.h"

/* ---- primitives (exported for the tests) -------------------------------- */
typedef struct { u32 h[5]; u32 total; u32 fill; u8 buf[64]; } sha1_ctx;
void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const u8 *p, u32 n);
void sha1_final(sha1_ctx *c, u8 out[20]);

typedef struct { sha1_ctx in, out; } hmac_ctx;       /* keyed pad states */
void hmac_sha1_key(hmac_ctx *k, const u8 *key, u32 keylen);
void hmac_sha1(const hmac_ctx *k, const u8 *a, u32 alen, const u8 *b, u32 blen, u8 out[20]);

void aes128_encrypt(const u8 key[16], const u8 in[16], u8 out[16]);
void aes128_decrypt(const u8 key[16], const u8 in[16], u8 out[16]);
/* RFC 3394 unwrap; n = wrapped length (multiple of 8, >= 24), out gets n - 8
   bytes. Returns 0, or -1 if the integrity check fails. */
int aes_key_unwrap(const u8 kek[16], const u8 *in, u32 n, u8 *out);

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32). About 16k SHA-1 blocks:
   call it before XNU starts (it blocks the monitor loop for a second or so). */
void wpa2_pmk(const char *pass, const u8 *ssid, u32 ssid_len, u8 pmk[32]);
/* PTK (first 48 bytes: KCK, KEK, TK) from PMK, addresses and nonces. */
void wpa2_ptk(const u8 pmk[32], const u8 sta[6], const u8 ap[6],
              const u8 snonce[32], const u8 anonce[32], u8 ptk[48]);
/* MD5 / HMAC-MD5 / RC4-drop-256: the WPA v1 (TKIP) EAPOL-Key algorithms.
   hmac_md5 takes a 16-byte KCK; wpa_rc4_skip uses IV||KEK (32 bytes). */
void md5_hash(const u8 *p, u32 n, u8 out[16]);
void hmac_md5(const u8 key[16], const u8 *a, u32 alen, const u8 *b, u32 blen, u8 out[16]);
void wpa_rc4_skip(const u8 key[32], u32 keylen, u32 skip, u8 *data, u32 len);
/* WPA v1 uses a 64-byte PTK: KCK 16, KEK 16, TKIP TK 32. */
void wpa1_ptk(const u8 pmk[32], const u8 sta[6], const u8 ap[6],
              const u8 snonce[32], const u8 anonce[32], u8 ptk[64]);

/* ---- 4-way / group handshake ------------------------------------------- */
enum {
    WPA2_EV_NONE = 0,
    WPA2_EV_M2_SENT,        /* reply = message 2 */
    WPA2_EV_KEYS,           /* reply = message 4: install TK + GTK, link is up */
    WPA2_EV_GTK,            /* reply = group message 2: install the new GTK */
};
enum {
    WPA2_ERR_SHORT = -1, WPA2_ERR_TYPE = -2, WPA2_ERR_REPLAY = -3, WPA2_ERR_MIC = -4,
    WPA2_ERR_STATE = -5, WPA2_ERR_KEYDATA = -6, WPA2_ERR_NOGTK = -7, WPA2_ERR_SPACE = -8,
    WPA2_ERR_NONCE = -9,
    WPA2_ERR_LONG = -10,            /* EAPOL frame longer than the MIC scratch buffer */
};

typedef struct {
    u8  pmk[32];
    u8  sta[6], ap[6];
    u8  snonce[32], anonce[32];
    u8  kck[16], kek[16], tk[32];    /* TK: 16 CCMP or 32 TKIP bytes */
    u8  gtk[32];
    u8  gtk_len, gtk_idx;
    u8  rsc[8];             /* GTK receive sequence counter from message 3 */
    u8  have_ptk, have_replay, done;
    u8  wpa1;               /* WPA v1: vendor IE, HMAC-MD5, RC4 key data, 64-byte TKIP PTK */
    u8  group_cipher;       /* RSN suite type in our IE: 4 = CCMP (default), 2 = TKIP */
    u8  replay[8];          /* last accepted replay counter (big endian) */
    u8  own_ie[40];         /* RSN IE exactly as the firmware sent it in the association */
    u8  own_ie_len;         /* request (CONNECT event); 0 = build our own (wpa2_rsn_ie) */
} wpa2_t;

void wpa2_start(wpa2_t *w, const u8 pmk[32], const u8 sta[6], const u8 ap[6],
                const u8 snonce[32]);
/* WPA v1 (vendor WPA IE, TKIP pairwise): same state, different key hierarchy. */
void wpa1_start(wpa2_t *w, const u8 pmk[32], const u8 sta[6], const u8 ap[6],
                const u8 snonce[32]);
/* The RSN IE that goes into message 2. It must equal the one in our
   association request (the AR6014 firmware builds that from WMI_CONNECT). */
u32 wpa2_rsn_ie(const wpa2_t *w, u8 out[22]);
/* The WPA v1 vendor IE (0xDD 00:50:F2:01) for message 2. */
u32 wpa1_ie(const wpa2_t *w, u8 out[26]);
/*
 * Feed one EAPOL frame, starting at the 802.1X header (version, type, length).
 * Returns a WPA2_EV_* (>= 0) or WPA2_ERR_* (< 0). When the event is not NONE,
 * `out` holds the EAPOL reply (*outlen bytes, again from the 802.1X header)
 * that has to be sent to the AP before the keys are installed.
 */
int wpa2_rx_eapol(wpa2_t *w, const u8 *eapol, u32 len, u8 *out, u32 cap, u32 *outlen);

/* An AP's RSN IE (from its beacon / probe response), parsed. */
typedef struct {
    u8 ok;                  /* well-formed up to the AKM list */
    u8 group;               /* group cipher suite type (00-0F-AC-n): 2 TKIP, 4 CCMP */
    u8 ccmp, tkip;          /* offered as pairwise */
    u8 psk, psk256, sae;    /* AKMs: 2 WPA2-PSK, 6 PSK-SHA256, 8 WPA3-SAE */
    u8 wpa1;                /* vendor WPA IE (WPA v1) */
    u8 n_akm;
    u8 mfpc, mfpr;          /* RSN capabilities: management frame protection capable / required */
} rsn_info_t;
enum {
    RSN_MODE_WPA2 = 0,      /* plain WPA2-PSK */
    RSN_MODE_MIXED,         /* WPA2 + WPA3 transition: we join with WPA2-PSK */
    RSN_MODE_WPA3_ONLY,     /* SAE only, or PMF required: the AR6014 cannot join */
    RSN_MODE_OTHER,         /* no PSK at all (802.1X / enterprise) or malformed */
    RSN_MODE_WPA1,          /* WPA v1-PSK: vendor IE, TKIP pairwise */
};
/* ie: the element (0x30, len, ...), n: bytes available. Returns RSN_MODE_*. */
int wpa2_rsn_parse(const u8 *ie, u32 n, rsn_info_t *r);
/* WPA v1 vendor IE (0xDD, len, 00:50:F2:01, ...). Same rsn_info_t. */
int wpa1_ie_parse(const u8 *ie, u32 n, rsn_info_t *r);
