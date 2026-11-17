/*
 * ARM9 side of the network mailbox (netmbox_t, boot/common/wifi_proto.h).
 * Pure C: unit-tested on the host together with the ARM11 side
 * (tools/ios3ds-wifi/shim/net_core.h).
 *
 * The ARM9 sees FCRAM uncached, so plain loads/stores are enough here; the
 * ordering rule is "slot first, index last".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wifi.h"

void net_mbox_init(volatile netmbox_t *m, u32 backend, const u8 mac[6])
{
    /* header lines only: the slots are dead until an index moves */
    memset((void *)m, 0, offsetof(netmbox_t, rx));
    for (int i = 0; i < 6; i++) m->mac[i] = mac ? mac[i] : 0;
    m->backend = (u16)backend;
    m->version = NET_VERSION;
    m->magic = NET_MAGIC;       /* last: the ARM11 ignores the mailbox before */
}

int net_mbox_push_rx(volatile netmbox_t *m, const u8 *frame, u32 len)
{
    if (len < NET_FRAME_MIN || len > NET_FRAME_MAX) return -2;
    u32 head = m->rx_head, tail = m->rx_tail;
    /* a tail outside [head - SLOTS, head] means the ARM11 side is not (yet)
       sane, e.g. XNU has not zeroed its line: treat as full */
    if (!net_ring_sane(head, tail) || head - tail == NET_SLOTS) {
        m->a9_rxdrop++;
        return -1;
    }
    volatile netslot_t *s = &m->rx[head % NET_SLOTS];
    memcpy((void *)s->data, frame, len);
    s->flags = 0;
    s->len = (u16)len;
    m->a9_rxframes++;
    m->rx_head = head + 1;
    return 0;
}

u32 net_mbox_peek_tx(volatile netmbox_t *m, u8 *frame, u32 cap)
{
    u32 head = m->tx_head, tail = m->tx_tail;
    if (head == tail) return 0;
    if (!net_ring_sane(head, tail)) {
        /* index garbage from the other side: resynchronise, drop everything */
        m->tx_tail = head;
        return 0;
    }
    volatile netslot_t *s = &m->tx[tail % NET_SLOTS];
    u32 len = s->len;
    if (len < NET_FRAME_MIN || len > NET_FRAME_MAX || len > cap) {
        m->tx_tail = tail + 1;      /* malformed slot: skip it */
        return 0;
    }
    memcpy(frame, (const void *)s->data, len);
    return len;
}

void net_mbox_tx_done(volatile netmbox_t *m)
{
    if (m->tx_head != m->tx_tail) {
        m->a9_txframes++;
        m->tx_tail = m->tx_tail + 1;
    }
}
