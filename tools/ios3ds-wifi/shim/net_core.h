/*
 * ARM11 (kernel shim) side of the network mailbox, without any kernel or CPU
 * dependency so the host tests can run it against the ARM9 side
 * (boot/arm9/wifi/wifi_ring.c). The includer provides the cache hooks:
 *
 *   NET_DINVAL(p, len)   drop cached copies of lines the ARM9 wrote
 *   NET_DCLEAN(p, len)   write lines we changed back to RAM
 *
 * See boot/common/wifi_proto.h for the protocol.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "wifi_proto.h"

#ifndef NET_DINVAL
#error "define NET_DINVAL(p, len) and NET_DCLEAN(p, len) before including net_core.h"
#endif

/* Build the outgoing netio_t: status + as many pending RX frames as fit.
   Nothing is consumed. */
static void net_core_fill(volatile netmbox_t *m, netio_t *io)
{
    uint32_t off = 0, n = 0;
    for (uint32_t i = 0; i < 32; i += 4) *(uint32_t *)((uint8_t *)io + i) = 0;
    io->magic = NETIO_MAGIC;
    io->version = NET_VERSION;
    NET_DINVAL(m, 32);                          /* line 0 */
    NET_DINVAL(&m->tx_tail, 32);                /* line 2 */
    if (m->magic != NET_MAGIC || m->version != NET_VERSION) {
        io->link = NETIO_LINK_NONE;
        return;
    }
    uint32_t head = m->rx_head, tail = m->rx_tail;
    if (!net_ring_sane(head, tail)) {
        /* our tail is garbage (first use / ARM9 restarted): drop the backlog */
        tail = head;
        m->rx_tail = tail;
        NET_DCLEAN(&m->rx_tail, 32);
    }
    uint32_t seq = tail;
    for (; seq != head; seq++) {
        volatile netslot_t *s = &m->rx[seq % NET_SLOTS];
        NET_DINVAL(s, NET_SLOT_SIZE);
        uint32_t len = s->len;
        if (len < NET_FRAME_MIN || len > NET_FRAME_MAX) continue;   /* skip, still acked */
        if (netio_put(io->data, NETIO_DATA, &off, (const uint8_t *)s->data, len) != 0) break;
        n++;
    }
    io->rx_next = seq;
    io->rx_pending = head - seq;
    io->nframes = (uint16_t)n;
    io->link = (uint16_t)(m->link ? 1 : 0);
    for (int i = 0; i < 6; i++) io->mac[i] = m->mac[i];
    io->backend = m->backend;
    io->state = (uint16_t)m->state;
    io->tx_free = (uint16_t)net_ring_free(m->tx_head, m->tx_tail);
}

/* Apply a netio_t written by userland (magic == NETIO_CMD): ack + TX.
   Returns the number of TX frames accepted. */
static uint32_t net_core_apply(volatile netmbox_t *m, const netio_t *io)
{
    uint32_t accepted = 0;
    if (io->magic != NETIO_CMD || io->version != NET_VERSION) return 0;
    NET_DINVAL(m, 32);
    NET_DINVAL(&m->tx_tail, 32);
    if (m->magic != NET_MAGIC || m->version != NET_VERSION) return 0;
    m->a11_calls++;
    if (!(io->rx_pending & NETIO_F_NOACK)) {
        uint32_t head = m->rx_head, tail = m->rx_tail;
        /* only ever move forward, and never past what the ARM9 has pushed */
        if (net_ring_sane(head, tail) && io->rx_next - tail <= head - tail)
            m->rx_tail = io->rx_next;
    }
    uint32_t off = 0, len = 0;
    for (uint32_t i = 0; i < io->nframes; i++) {
        const uint8_t *f = netio_get(io->data, NETIO_DATA, &off, &len);
        if (!f) {                               /* malformed batch: stop */
            m->a11_txdrop += io->nframes - i;
            break;
        }
        uint32_t head = m->tx_head, tail = m->tx_tail;
        if (net_ring_free(head, tail) == 0) {
            m->a11_txdrop++;
            continue;
        }
        volatile netslot_t *s = &m->tx[head % NET_SLOTS];
        for (uint32_t j = 0; j < len; j++) s->data[j] = f[j];
        s->flags = 0;
        s->len = (uint16_t)len;
        NET_DCLEAN(s, 4 + len);
        m->tx_head = head + 1;                  /* index last ... */
        NET_DCLEAN(&m->rx_tail, 32);            /* ... and visible to the ARM9 per frame */
        accepted++;
    }
    NET_DCLEAN(&m->rx_tail, 32);
    return accepted;
}
