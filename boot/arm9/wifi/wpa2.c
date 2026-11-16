/*
 * WPA2-PSK supplicant core: SHA-1, HMAC, PBKDF2, the 802.11i PRF, AES-128,
 * RFC 3394 key unwrap and the EAPOL-Key 4-way + group handshakes.
 * Written from the standards (FIPS 180-1, RFC 2104/2898/3394, FIPS-197,
 * IEEE 802.11i-2004 8.5); no third-party code. See wpa2.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wpa2.h"

/* ------------------------------------------------------------------------ */
/* SHA-1 */

static u32 rol(u32 v, int n) { return v << n | v >> (32 - n); }

static void sha1_block(sha1_ctx *c, const u8 *p)
{
    u32 w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6u; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->fill = 0;
}

void sha1_update(sha1_ctx *c, const u8 *p, u32 n)
{
    c->total += n;
    while (n) {
        u32 k = 64 - c->fill;
        if (k > n) k = n;
        memcpy(c->buf + c->fill, p, k);
        c->fill += k; p += k; n -= k;
        if (c->fill == 64) {
            sha1_block(c, c->buf);
            c->fill = 0;
        }
    }
}

void sha1_final(sha1_ctx *c, u8 out[20])
{
    u32 bits = c->total << 3, hi = c->total >> 29;
    u8 pad[72];
    u32 n = (c->fill < 56 ? 56 : 120) - c->fill;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    pad[n + 0] = (u8)(hi >> 24); pad[n + 1] = (u8)(hi >> 16); pad[n + 2] = (u8)(hi >> 8); pad[n + 3] = (u8)hi;
    pad[n + 4] = (u8)(bits >> 24); pad[n + 5] = (u8)(bits >> 16); pad[n + 6] = (u8)(bits >> 8); pad[n + 7] = (u8)bits;
    sha1_update(c, pad, n + 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (u8)(c->h[i] >> 24); out[4 * i + 1] = (u8)(c->h[i] >> 16);
        out[4 * i + 2] = (u8)(c->h[i] >> 8); out[4 * i + 3] = (u8)c->h[i];
    }
}

/* ------------------------------------------------------------------------ */
/* HMAC-SHA1 with the two pad blocks hashed once per key */

void hmac_sha1_key(hmac_ctx *k, const u8 *key, u32 keylen)
{
    u8 kb[64], pad[64];
    memset(kb, 0, sizeof(kb));
    if (keylen > 64) {
        sha1_ctx c;
        sha1_init(&c);
        sha1_update(&c, key, keylen);
        sha1_final(&c, kb);
    } else {
        memcpy(kb, key, keylen);
    }
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    sha1_init(&k->in);
    sha1_update(&k->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5C;
    sha1_init(&k->out);
    sha1_update(&k->out, pad, 64);
}

void hmac_sha1(const hmac_ctx *k, const u8 *a, u32 alen, const u8 *b, u32 blen, u8 out[20])
{
    sha1_ctx c = k->in;
    u8 inner[20];
    if (alen) sha1_update(&c, a, alen);
    if (blen) sha1_update(&c, b, blen);
    sha1_final(&c, inner);
    c = k->out;
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

/* ------------------------------------------------------------------------ */
/* key derivation */

void wpa2_pmk(const char *pass, const u8 *ssid, u32 ssid_len, u8 pmk[32])
{
    hmac_ctx k;
    u8 salt[36], u[20], t[20], out[40];
    if (ssid_len > 32) ssid_len = 32;
    hmac_sha1_key(&k, (const u8 *)pass, strlen(pass));
    memcpy(salt, ssid, ssid_len);
    for (u32 blk = 1; blk <= 2; blk++) {
        salt[ssid_len] = 0; salt[ssid_len + 1] = 0; salt[ssid_len + 2] = 0; salt[ssid_len + 3] = (u8)blk;
        hmac_sha1(&k, salt, ssid_len + 4, NULL, 0, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            hmac_sha1(&k, u, 20, NULL, 0, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        memcpy(out + (blk - 1) * 20, t, 20);
    }
    memcpy(pmk, out, 32);
}

static const u8 *lower(const u8 *a, const u8 *b, u32 n) { return memcmp(a, b, n) <= 0 ? a : b; }
static const u8 *higher(const u8 *a, const u8 *b, u32 n) { return memcmp(a, b, n) <= 0 ? b : a; }

void wpa2_ptk(const u8 pmk[32], const u8 sta[6], const u8 ap[6],
              const u8 snonce[32], const u8 anonce[32], u8 ptk[48])
{
    static const char label[] = "Pairwise key expansion";   /* incl. NUL = 23 bytes */
    hmac_ctx k;
    u8 data[23 + 12 + 64 + 1], d[20], out[60];
    memcpy(data, label, 23);
    memcpy(data + 23, lower(sta, ap, 6), 6);
    memcpy(data + 29, higher(sta, ap, 6), 6);
    memcpy(data + 35, lower(snonce, anonce, 32), 32);
    memcpy(data + 67, higher(snonce, anonce, 32), 32);
    hmac_sha1_key(&k, pmk, 32);
    for (u8 i = 0; i < 3; i++) {
        data[99] = i;
        hmac_sha1(&k, data, 100, NULL, 0, d);
        memcpy(out + i * 20, d, 20);
    }
    memcpy(ptk, out, 48);
}

/* ------------------------------------------------------------------------ */
/* AES-128 (tables generated once; only used for the 40-byte GTK unwrap) */

static u8 sbox[256], isbox[256];
static u8 aes_ready;

static u8 rol8(u8 v, int n) { return (u8)(v << n | v >> (8 - n)); }
static u8 xt(u8 v) { return (u8)(v << 1 ^ (v & 0x80 ? 0x1B : 0)); }

static void aes_tables(void)
{
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ xt(p));                    /* p *= 3 */
        q ^= (u8)(q << 1); q ^= (u8)(q << 2); q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;                /* q /= 3 */
        u8 x = (u8)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4) ^ 0x63);
        sbox[p] = x;
        isbox[x] = p;
    } while (p != 1);
    sbox[0] = 0x63;
    isbox[0x63] = 0;
    aes_ready = 1;
}

static void aes_expand(const u8 key[16], u8 rk[176])
{
    u8 rcon = 1;
    if (!aes_ready) aes_tables();
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            u8 t0 = t[0];
            t[0] = (u8)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[t0];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}

static void add_rk(u8 s[16], const u8 *rk) { for (int i = 0; i < 16; i++) s[i] ^= rk[i]; }

static u8 gmul(u8 a, u8 b)
{
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

void aes128_encrypt(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 rk[176], s[16], t[16];
    aes_expand(key, rk);
    memcpy(s, in, 16);
    add_rk(s, rk);
    for (int r = 1; r <= 10; r++) {
        for (int c = 0; c < 4; c++)             /* SubBytes + ShiftRows */
            for (int row = 0; row < 4; row++) t[4 * c + row] = sbox[s[4 * ((c + row) & 3) + row]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {       /* MixColumns */
                u8 *a = t + 4 * c, x = (u8)(a[0] ^ a[1] ^ a[2] ^ a[3]);
                s[4 * c + 0] = (u8)(a[0] ^ x ^ xt((u8)(a[0] ^ a[1])));
                s[4 * c + 1] = (u8)(a[1] ^ x ^ xt((u8)(a[1] ^ a[2])));
                s[4 * c + 2] = (u8)(a[2] ^ x ^ xt((u8)(a[2] ^ a[3])));
                s[4 * c + 3] = (u8)(a[3] ^ x ^ xt((u8)(a[3] ^ a[0])));
            }
        } else {
            memcpy(s, t, 16);
        }
        add_rk(s, rk + 16 * r);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 rk[176], s[16], t[16];
    aes_expand(key, rk);
    memcpy(s, in, 16);
    add_rk(s, rk + 160);
    for (int r = 9; r >= 0; r--) {
        for (int c = 0; c < 4; c++)             /* InvShiftRows + InvSubBytes */
            for (int row = 0; row < 4; row++) t[4 * ((c + row) & 3) + row] = isbox[s[4 * c + row]];
        add_rk(t, rk + 16 * r);
        if (r > 0) {
            for (int c = 0; c < 4; c++) {       /* InvMixColumns */
                u8 *a = t + 4 * c;
                s[4 * c + 0] = (u8)(gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9));
                s[4 * c + 1] = (u8)(gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13));
                s[4 * c + 2] = (u8)(gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11));
                s[4 * c + 3] = (u8)(gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14));
            }
        } else {
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
}

int aes_key_unwrap(const u8 kek[16], const u8 *in, u32 n, u8 *out)
{
    if (n < 24 || (n & 7)) return -1;
    u32 blocks = n / 8 - 1;
    u8 a[8], b[16];
    memcpy(a, in, 8);
    memmove(out, in + 8, n - 8);
    for (int j = 5; j >= 0; j--) {
        for (u32 i = blocks; i >= 1; i--) {
            u32 t = blocks * (u32)j + i;
            memcpy(b, a, 8);
            b[7] ^= (u8)t; b[6] ^= (u8)(t >> 8); b[5] ^= (u8)(t >> 16); b[4] ^= (u8)(t >> 24);
            memcpy(b + 8, out + 8 * (i - 1), 8);
            aes128_decrypt(kek, b, b);
            memcpy(a, b, 8);
            memcpy(out + 8 * (i - 1), b + 8, 8);
        }
    }
    u8 bad = 0;
    for (int i = 0; i < 8; i++) bad |= a[i] ^ 0xA6;
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------------ */
/* EAPOL-Key (IEEE 802.11i 8.5.2), offsets from the 802.1X header */

#define EK_TYPE     1       /* 3 = EAPOL-Key */
#define EK_BODYLEN  2
#define EK_DESC     4       /* 2 = RSN */
#define EK_INFO     5
#define EK_KEYLEN   7
#define EK_REPLAY   9
#define EK_NONCE    17
#define EK_RSC      65
#define EK_MIC      81
#define EK_DATALEN  97
#define EK_DATA     99

#define KI_VER_MASK 0x0007
#define KI_VER_AES  0x0002  /* HMAC-SHA1 MIC + AES key wrap */
#define KI_PAIRWISE 0x0008
#define KI_INSTALL  0x0040
#define KI_ACK      0x0080
#define KI_MIC      0x0100
#define KI_SECURE   0x0200
#define KI_ERROR    0x0400
#define KI_REQUEST  0x0800
#define KI_ENCDATA  0x1000

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

void wpa2_start(wpa2_t *w, const u8 pmk[32], const u8 sta[6], const u8 ap[6],
                const u8 snonce[32])
{
    memset(w, 0, sizeof(*w));
    memcpy(w->pmk, pmk, 32);
    memcpy(w->sta, sta, 6);
    memcpy(w->ap, ap, 6);
    memcpy(w->snonce, snonce, 32);
    w->group_cipher = 4;
}

u32 wpa2_rsn_ie(const wpa2_t *w, u8 out[22])
{
    static const u8 ie[22] = {
        0x30, 0x14, 0x01, 0x00,                 /* RSN, len 20, version 1 */
        0x00, 0x0F, 0xAC, 0x04,                 /* group cipher (patched below) */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,     /* 1 pairwise: CCMP */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,     /* 1 AKM: PSK */
        0x00, 0x00,                             /* capabilities */
    };
    memcpy(out, ie, 22);
    out[7] = w->group_cipher;
    return 22;
}

/* MIC over the whole EAPOL frame with the MIC field taken as zero */
static void eapol_mic(const u8 kck[16], const u8 *f, u32 len, u8 mic[16])
{
    static const u8 zero[16];
    hmac_ctx k;
    sha1_ctx c;
    u8 d[20];
    hmac_sha1_key(&k, kck, 16);
    c = k.in;
    sha1_update(&c, f, EK_MIC);
    sha1_update(&c, zero, 16);
    sha1_update(&c, f + EK_MIC + 16, len - (EK_MIC + 16));
    sha1_final(&c, d);
    c = k.out;
    sha1_update(&c, d, 20);
    sha1_final(&c, d);
    memcpy(mic, d, 16);
}

static int build_reply(wpa2_t *w, const u8 *req, u32 info, const u8 *nonce,
                       const u8 *data, u32 dlen, u8 *out, u32 cap, u32 *outlen)
{
    u32 total = EK_DATA + dlen;
    if (total > cap) return WPA2_ERR_SPACE;
    memset(out, 0, total);
    out[0] = req[0] < 2 ? 1 : 2;        /* answer 802.1X-2001 with version 1 */
    out[EK_TYPE] = 3;
    out[EK_BODYLEN] = (u8)((total - 4) >> 8);
    out[EK_BODYLEN + 1] = (u8)(total - 4);
    out[EK_DESC] = 2;
    out[EK_INFO] = (u8)(info >> 8);
    out[EK_INFO + 1] = (u8)info;
    memcpy(out + EK_REPLAY, req + EK_REPLAY, 8);
    if (nonce) memcpy(out + EK_NONCE, nonce, 32);
    out[EK_DATALEN] = (u8)(dlen >> 8);
    out[EK_DATALEN + 1] = (u8)dlen;
    if (dlen) memcpy(out + EK_DATA, data, dlen);
    eapol_mic(w->kck, out, total, out + EK_MIC);
    *outlen = total;
    return 0;
}

/* Find the GTK KDE (DD len 00-0F-AC 01 keyid 00 gtk...) in decrypted key data */
static int find_gtk(wpa2_t *w, const u8 *kd, u32 n)
{
    u32 i = 0;
    while (i + 2 <= n) {
        u32 type = kd[i], l = kd[i + 1];
        if (type == 0xDD && l == 0) break;              /* padding */
        if (i + 2 + l > n) return WPA2_ERR_KEYDATA;
        if (type == 0xDD && l >= 6 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0F &&
            kd[i + 4] == 0xAC && kd[i + 5] == 0x01) {
            u32 glen = l - 6;
            if (glen != 16 && glen != 32) return WPA2_ERR_KEYDATA;
            w->gtk_idx = kd[i + 6] & 3;
            w->gtk_len = (u8)glen;
            memcpy(w->gtk, kd + i + 8, glen);
            return 0;
        }
        i += 2 + l;
    }
    return WPA2_ERR_NOGTK;
}

int wpa2_rx_eapol(wpa2_t *w, const u8 *f, u32 len, u8 *out, u32 cap, u32 *outlen)
{
    u8 kd[256], mic[16];
    *outlen = 0;
    if (len < EK_DATA) return WPA2_ERR_SHORT;
    if (f[EK_TYPE] != 3 || f[EK_DESC] != 2) return WPA2_ERR_TYPE;
    u32 body = be16(f + EK_BODYLEN), info = be16(f + EK_INFO), dlen = be16(f + EK_DATALEN);
    if (body + 4 > len || body + 4 < EK_DATA || dlen > body + 4 - EK_DATA) return WPA2_ERR_SHORT;
    len = body + 4;                             /* ignore trailing padding */
    if ((info & KI_VER_MASK) != KI_VER_AES) return WPA2_ERR_TYPE;
    if (info & (KI_REQUEST | KI_ERROR) || !(info & KI_ACK)) return WPA2_ERR_TYPE;
    /* replay counter must increase (802.11i 8.5.2 f); the first one is free */
    if (w->have_replay && memcmp(f + EK_REPLAY, w->replay, 8) <= 0) return WPA2_ERR_REPLAY;

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
        /* message 1: ANonce. Unauthenticated, so do not latch the replay
           counter here (a forged M1 must not lock out the real AP). */
        u8 ptk[48], ie[22];
        memcpy(w->anonce, f + EK_NONCE, 32);
        wpa2_ptk(w->pmk, w->sta, w->ap, w->snonce, w->anonce, ptk);
        memcpy(w->kck, ptk, 16);
        memcpy(w->kek, ptk + 16, 16);
        memcpy(w->tk, ptk + 32, 16);
        w->have_ptk = 1;
        w->done = 0;
        u32 ielen = wpa2_rsn_ie(w, ie);
        int r = build_reply(w, f, KI_VER_AES | KI_PAIRWISE | KI_MIC, w->snonce, ie, ielen,
                            out, cap, outlen);
        return r ? r : WPA2_EV_M2_SENT;
    }

    /* everything else is MIC-protected with our KCK */
    if (!w->have_ptk || !(info & KI_MIC)) return WPA2_ERR_STATE;
    eapol_mic(w->kck, f, len, mic);
    u8 bad = 0;
    for (int i = 0; i < 16; i++) bad |= mic[i] ^ f[EK_MIC + i];
    if (bad) return WPA2_ERR_MIC;
    if (info & KI_PAIRWISE) {
        if (!(info & KI_INSTALL)) return WPA2_ERR_TYPE;
        if (memcmp(f + EK_NONCE, w->anonce, 32) != 0) return WPA2_ERR_NONCE;
    } else if (!w->done) {
        return WPA2_ERR_STATE;                  /* group rekey before the 4-way finished */
    }
    if (!(info & KI_ENCDATA) || dlen < 24 || (dlen & 7) || dlen - 8 > sizeof(kd))
        return WPA2_ERR_KEYDATA;
    if (aes_key_unwrap(w->kek, f + EK_DATA, dlen, kd) != 0) return WPA2_ERR_KEYDATA;
    int r = find_gtk(w, kd, dlen - 8);
    if (r) return r;
    memcpy(w->rsc, f + EK_RSC, 8);
    memcpy(w->replay, f + EK_REPLAY, 8);
    w->have_replay = 1;
    if (info & KI_PAIRWISE) {
        r = build_reply(w, f, KI_VER_AES | KI_PAIRWISE | KI_MIC | KI_SECURE, NULL, NULL, 0,
                        out, cap, outlen);
        if (r) return r;
        w->done = 1;
        return WPA2_EV_KEYS;
    }
    r = build_reply(w, f, KI_VER_AES | KI_MIC | KI_SECURE, NULL, NULL, 0, out, cap, outlen);
    return r ? r : WPA2_EV_GTK;
}
