/*
 * Ethernet II <-> AR6K "WMI data" frames. Pure C, unit-tested on the host.
 *
 * The AR60xx firmware exchanges 802.3 frames with an LLC/SNAP header behind a
 * two-byte WMI data header (layout as used by dsiwifi's data_send_* /
 * data_handle_pkt and described in GBATEK "DSi Atheros Wifi - MBOX"):
 *
 *   TX: 00 1C | dst[6] src[6] | be16 length | AA AA 03 00 00 00 | ethertype | payload
 *   RX: rssi info | dst[6] src[6] | be16 length | AA AA 03 00 00 00 | ethertype | payload
 *
 * iOS (IOUserEthernet / IOEthernetInterface) wants plain Ethernet II.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"

static const u8 snap_rfc1042[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
static const u8 snap_bridge[6]  = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8 };

u32 wifi_frame_to_wmi(const u8 *eth, u32 len, u8 *out, u32 cap)
{
    if (len < NET_FRAME_MIN || len > NET_FRAME_MAX) return 0;
    u32 payload = len - 14;
    u32 total = WMI_DATA_HDR + 14 + LLC_SNAP_LEN + payload;
    if (total > cap) return 0;
    out[0] = 0x00;                      /* WMI data header as sent by dsiwifi */
    out[1] = 0x1C;
    memcpy(out + 2, eth, 12);           /* dst, src */
    u32 l = LLC_SNAP_LEN + payload;     /* 802.3 length field */
    out[14] = (u8)(l >> 8);
    out[15] = (u8)l;
    memcpy(out + 16, snap_rfc1042, 6);
    out[22] = eth[12];                  /* ethertype */
    out[23] = eth[13];
    memcpy(out + 24, eth + 14, payload);
    return total;
}

u32 wifi_frame_from_wmi(const u8 *wmi, u32 len, u8 *out, u32 cap, u16 *ethertype)
{
    if (len < WMI_DATA_HDR + 14) return 0;
    const u8 *f = wmi + WMI_DATA_HDR;
    u32 flen = len - WMI_DATA_HDR;
    u32 tl = (u32)f[12] << 8 | f[13];
    if (tl >= 0x600) {
        /* already Ethernet II */
        if (flen > cap || flen > NET_FRAME_MAX) return 0;
        memcpy(out, f, flen);
        if (ethertype) *ethertype = (u16)tl;
        return flen;
    }
    /* 802.3: tl = bytes after the length field (the firmware pads the mailbox
       packet, so trust the field, not the transfer size) */
    if (tl > flen - 14) return 0;
    const u8 *llc = f + 14;
    if (tl >= LLC_SNAP_LEN &&
        (memcmp(llc, snap_rfc1042, 6) == 0 || memcmp(llc, snap_bridge, 6) == 0)) {
        u32 payload = tl - LLC_SNAP_LEN;
        u32 total = 14 + payload;
        if (total > cap || total > NET_FRAME_MAX) return 0;
        memcpy(out, f, 12);
        out[12] = llc[6];
        out[13] = llc[7];
        memcpy(out + 14, llc + 8, payload);
        if (ethertype) *ethertype = (u16)((u32)llc[6] << 8 | llc[7]);
        return total;
    }
    /* raw 802.3 / non-SNAP LLC (e.g. spanning tree): pass with its length field */
    u32 total = 14 + tl;
    if (total > cap || total > NET_FRAME_MAX) return 0;
    memcpy(out, f, total);
    if (ethertype) *ethertype = 0;
    return total;
}

/* HTC credit reports in an RX trailer: {id=1, len, {endpoint, credits}...},
   possibly followed by other records. Malformed records stop the parser: a
   torn trailer must never credit an endpoint twice. */
u32 wifi_htc_credits(const u8 *t, u32 n, u8 credits[8])
{
    u32 i = 0, got = 0;
    while (i + 2 <= n) {
        u32 id = t[i], l = t[i + 1];
        if (i + 2 + l > n) break;
        if (id == WMI_HTC_REC_CREDITS) {
            for (u32 k = 0; k + 1 < l; k += 2) {
                u32 ep = t[i + 2 + k], cr = t[i + 2 + k + 1];
                if (ep >= 8) continue;
                u32 v = (u32)credits[ep] + cr;
                credits[ep] = (u8)(v > 255 ? 255 : v);
                got += cr;
            }
        }
        i += 2 + l;
    }
    return got;
}
