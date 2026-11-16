/*
 * Emulator backend: the "ctr-vnic" paravirtual NIC of the ios3ds QEMU board
 * (register spec: boot/common/wifi_proto.h and docs/wifi.md section 7).
 * It stands in for the AR6014 + access point: QEMU's user-mode network gives
 * iOS DHCP, DNS and NAT, so everything above the ARM9 driver can be tested.
 *
 * Only used when wifi.cfg says backend=qemu: the address is not real 3DS
 * hardware, and probing it on a console could data-abort.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"

#define REG(off) (*(vu32 *)(VNIC_BASE + (off)))

static int vnic_err;

static int vnic_init(const wifi_cfg_t *cfg, u8 mac[6])
{
    (void)cfg;
    if (REG(VNIC_ID) != VNIC_ID_VALUE) {
        vnic_err = -1;          /* board without the device: the block reads 0 */
        return -1;
    }
    REG(VNIC_CTRL) = VNIC_CTRL_RESET;
    REG(VNIC_CTRL) = VNIC_CTRL_EN;
    u32 lo = REG(VNIC_MAC_LO), hi = REG(VNIC_MAC_HI);
    mac[0] = (u8)lo; mac[1] = (u8)(lo >> 8); mac[2] = (u8)(lo >> 16); mac[3] = (u8)(lo >> 24);
    mac[4] = (u8)hi; mac[5] = (u8)(hi >> 8);
    vnic_err = 0;
    return 0;
}

static u32 vnic_poll(void)
{
    return (REG(VNIC_STATUS) & VNIC_ST_LINK) ? NET_ST_UP : NET_ST_SCAN;
}

static int vnic_tx(const u8 *eth, u32 len)
{
    if (len > NET_FRAME_MAX) return -2;
    if (!(REG(VNIC_STATUS) & VNIC_ST_TXIDLE)) return -1;
    volatile u8 *b = (volatile u8 *)(VNIC_BASE + VNIC_TX_BUF);
    for (u32 i = 0; i < len; i++) b[i] = eth[i];
    REG(VNIC_TX_LEN) = len;
    return 0;
}

static u32 vnic_rx(u8 *eth, u32 cap)
{
    u32 len = REG(VNIC_RX_LEN);
    if (!len) return 0;
    if (len <= cap && len <= NET_FRAME_MAX) {
        volatile u8 *b = (volatile u8 *)(VNIC_BASE + VNIC_RX_BUF);
        for (u32 i = 0; i < len; i++) eth[i] = b[i];
    } else {
        len = 0;                /* oversized: drop it */
    }
    REG(VNIC_RX_ACK) = 1;
    return len;
}

static int vnic_last_error(void) { return vnic_err; }

const wifi_backend_t wifi_backend_vnic = {
    .name = "qemu-vnic", .id = NET_BACKEND_VNIC,
    .init = vnic_init, .poll = vnic_poll, .tx = vnic_tx, .rx = vnic_rx,
    .last_error = vnic_last_error,
};
