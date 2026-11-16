/*
 * ios3ds network mailbox: 802.3 Ethernet frames between the ARM9 monitor (which
 * owns the Wi-Fi chip) and iOS on the ARM11.
 *
 *   AR6014 / QEMU vnic <-> ARM9 (boot/arm9/wifi) <-> netmbox_t in FCRAM
 *     <-> XNU shim sysctl kern.ios3ds_net (tools/ios3ds-wifi/shim/net.c)
 *     <-> ios3ds-wifid (tools/ios3ds-wifi/daemon) <-> IOUserEthernet "en0"
 *
 * Shared by all three sides and by the host unit tests, so it is plain C with
 * no dependencies beyond <stdint.h>/<string.h>-style helpers defined here.
 * See docs/wifi.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- placement ------------------------------------------------------- */
/* FCRAM between the XNU log ring (0x20850000 + 256 KiB) and topOfKernelData
   (0x20900000). Same 1 MiB kernel section as the shim, so the kernel maps it
   at NET_VA and never hands it to the VM. */
#define NET_PA          0x208A0000u
#define NET_VA          0x808A0000u
#define NET_SIZE        0x10000u
#define NET_IO_OFF      0xE000u         /* netio_t scratch, owned by the ARM11 */

#define NET_MAGIC       0x4D54454Eu     /* 'NETM' */
#define NET_VERSION     1u
#define NET_SLOTS       16u             /* frames per direction */
#define NET_SLOT_SIZE   1536u           /* 48 cache lines */
#define NET_FRAME_MAX   1514u           /* 14-byte header + 1500 MTU, no FCS */
#define NET_FRAME_MIN   14u

/* netmbox_t.backend */
#define NET_BACKEND_NONE    0u
#define NET_BACKEND_AR6014  1u          /* real hardware */
#define NET_BACKEND_VNIC    2u          /* QEMU paravirtual NIC (docs/wifi.md 7) */
#define NET_BACKEND_LOOP    3u          /* test: echoes frames back */

/* netmbox_t.state: where the ARM9 driver is (also shown on the 3DS console) */
enum {
    NET_ST_OFF = 0,         /* no wifi.cfg / backend=off */
    NET_ST_POWER,           /* chip powered, SDIO enumerating */
    NET_ST_BMI,             /* talking to the ROM bootloader */
    NET_ST_FWLOAD,          /* uploading firmware parts */
    NET_ST_BOOT,            /* BMI_DONE sent, waiting for the firmware */
    NET_ST_HTC,             /* HTC service handshake */
    NET_ST_WMI,             /* WMI ready, configuring */
    NET_ST_SCAN,            /* connect command sent */
    NET_ST_AUTH,            /* associated, WPA2 4-way handshake running */
    NET_ST_UP,              /* link up: frames flow */
    NET_ST_FAILED,          /* see netmbox_t.err */
};

typedef struct {
    uint16_t len;           /* frame bytes in data[] */
    uint16_t flags;         /* reserved */
    uint8_t  data[NET_SLOT_SIZE - 4];
} netslot_t;

/*
 * Every 32-byte cache line has exactly one writer (same rule as blkmbox_t):
 * the ARM11 runs with the D-cache on and cleans/invalidates single lines, the
 * ARM9 sees FCRAM uncached. Indices only ever increase ("frames ever pushed");
 * slot = index % NET_SLOTS. A producer writes the slot first, the index last.
 */
typedef struct {
    /* line 0: ARM9 */
    uint32_t magic;         /* NET_MAGIC once the ARM9 serves the mailbox */
    uint32_t version;
    uint32_t rx_head;       /* frames pushed towards iOS */
    uint32_t link;          /* 1 = associated and keys installed */
    uint8_t  mac[6];        /* station address (valid when backend != NONE) */
    uint16_t backend;       /* NET_BACKEND_* */
    uint32_t state;         /* NET_ST_* */
    int32_t  err;           /* last driver error (negative), 0 = none */
    /* line 1: ARM11 */
    uint32_t rx_tail;       /* frames iOS has consumed */
    uint32_t tx_head;       /* frames pushed towards the network */
    uint32_t a11_calls;     /* sysctl calls (liveness) */
    uint32_t a11_txdrop;    /* frames dropped: TX ring full / bad length */
    uint32_t _r1[4];
    /* line 2: ARM9 */
    uint32_t tx_tail;       /* frames the ARM9 has sent */
    uint32_t a9_rxdrop;     /* frames dropped: RX ring full */
    uint32_t a9_rxframes, a9_txframes;
    uint32_t a9_polls;
    uint32_t _r2[3];
    /* line 3: ARM9 */
    char     ssid[32];      /* for status display, not NUL-terminated at 32 */
    /* slots */
    netslot_t rx[NET_SLOTS];    /* writer: ARM9 */
    netslot_t tx[NET_SLOTS];    /* writer: ARM11 */
} netmbox_t;

_Static_assert(sizeof(netslot_t) == NET_SLOT_SIZE, "netslot layout");
_Static_assert(offsetof(netmbox_t, rx_tail) == 32, "netmbox line 1");
_Static_assert(offsetof(netmbox_t, tx_tail) == 64, "netmbox line 2");
_Static_assert(offsetof(netmbox_t, rx) == 128, "netmbox slots");
_Static_assert(sizeof(netmbox_t) <= NET_IO_OFF, "netmbox overlaps the io buffer");

/* ---- userland view: sysctl kern.ios3ds_net ---------------------------- */
/*
 * One fixed-size read/write opaque sysctl does everything (fixed size because
 * that is what sysctl_io_opaque() supports without knowing struct sysctl_req):
 *
 *   read  (oldp):  header + the next pending RX frames, *peeked* (not consumed)
 *   write (newp):  magic = NETIO_CMD; rx_next = the rx_next value of the batch
 *                  being acknowledged (0 with NETIO_F_NOACK: no ack); nframes
 *                  TX frames in data[]
 *
 * A call may do both at once: old = fresh status + RX batch, new = ack of the
 * previous batch + TX. A plain read (e.g. `sysctl -a`) never loses frames, and
 * a repeated ack is harmless (acks are absolute sequence numbers).
 */
#define NET_SYSCTL_NAME "kern.ios3ds_net"
#define NETIO_SIZE      4096u
#define NETIO_MAGIC     0x4F49454Eu     /* 'NEIO': kernel -> user */
#define NETIO_CMD       0x444D434Eu     /* 'NCMD': user -> kernel */
#define NETIO_DATA      (NETIO_SIZE - 32u)
#define NETIO_LINK_NONE 0xFFFFu         /* ARM9 is not serving the mailbox */
#define NETIO_F_NOACK   1u              /* in (rx_pending field): rx_next is not an ack */

typedef struct {
    uint32_t magic;         /* NETIO_MAGIC out, NETIO_CMD in */
    uint32_t version;       /* NET_VERSION */
    uint32_t rx_next;       /* out: sequence number after this batch; write it
                               back to consume the batch. in: the ack */
    uint16_t nframes;       /* frames packed in data[] (out: RX, in: TX) */
    uint16_t link;          /* out: 1 = up, 0 = down, NETIO_LINK_NONE */
    uint8_t  mac[6];        /* out */
    uint16_t backend;       /* out: NET_BACKEND_* */
    uint16_t state;         /* out: NET_ST_* */
    uint16_t tx_free;       /* out: free TX slots before this call's TX */
    uint32_t rx_pending;    /* out: frames still queued behind this batch;
                               in: NETIO_F_* flags */
    uint8_t  data[NETIO_DATA];  /* { u16 len (LE); u8 frame[len]; pad to even } ... */
} netio_t;
_Static_assert(sizeof(netio_t) == NETIO_SIZE, "netio layout");

/* Append one frame to a packed buffer. Returns 0, or -1 if it does not fit
   or the length is not a valid Ethernet frame. *off is advanced on success. */
static inline int netio_put(uint8_t *data, uint32_t cap, uint32_t *off,
                            const uint8_t *frame, uint32_t len)
{
    uint32_t need = 2u + ((len + 1u) & ~1u);
    if (len < NET_FRAME_MIN || len > NET_FRAME_MAX) return -1;
    if (*off > cap || cap - *off < need) return -1;
    uint8_t *p = data + *off;
    p[0] = (uint8_t)len;
    p[1] = (uint8_t)(len >> 8);
    for (uint32_t i = 0; i < len; i++) p[2 + i] = frame[i];
    if (len & 1u) p[2 + len] = 0;
    *off += need;
    return 0;
}

/* Take the next frame out of a packed buffer of `used` bytes. Returns a
   pointer into data and sets *len, or NULL when the buffer is exhausted or
   malformed (never reads past `used`). */
static inline const uint8_t *netio_get(const uint8_t *data, uint32_t used,
                                       uint32_t *off, uint32_t *len)
{
    if (*off > used || used - *off < 2u) return NULL;
    uint32_t l = (uint32_t)data[*off] | ((uint32_t)data[*off + 1] << 8);
    uint32_t need = 2u + ((l + 1u) & ~1u);
    if (l < NET_FRAME_MIN || l > NET_FRAME_MAX || used - *off < need) return NULL;
    const uint8_t *p = data + *off + 2;
    *off += need;
    *len = l;
    return p;
}

/* Ring arithmetic on the free-running indices (wrap-safe). */
static inline uint32_t net_ring_used(uint32_t head, uint32_t tail) { return head - tail; }
static inline int net_ring_sane(uint32_t head, uint32_t tail) { return head - tail <= NET_SLOTS; }
static inline uint32_t net_ring_free(uint32_t head, uint32_t tail)
{
    return net_ring_sane(head, tail) ? NET_SLOTS - (head - tail) : 0;
}

/* ---- QEMU paravirtual NIC ("ctr-vnic", emulator only) ------------------ */
/*
 * Not real 3DS hardware. The QEMU board maps it in the shared IO block; the
 * ARM9 only touches it when wifi.cfg says backend=qemu (on a console an
 * unmapped IO address may data-abort). Register spec: docs/wifi.md section 7.
 */
#define VNIC_BASE       0x101F0000u
#define VNIC_ID         0x000u      /* RO  VNIC_ID_VALUE */
#define VNIC_CTRL       0x004u      /* RW  bit0 enable RX, bit1 reset (self-clearing) */
#define VNIC_STATUS     0x008u      /* RO  bit0 link up, bit1 RX frame waiting, bit2 TX idle */
#define VNIC_MAC_LO     0x00Cu      /* RO  mac[0..3], mac[0] in bits 0-7 */
#define VNIC_MAC_HI     0x010u      /* RO  mac[4..5] */
#define VNIC_RX_LEN     0x014u      /* RO  length of the frame in the RX buffer, 0 = none */
#define VNIC_RX_ACK     0x018u      /* WO  write 1: drop that frame, load the next */
#define VNIC_TX_LEN     0x01Cu      /* WO  write n: send n bytes of the TX buffer */
#define VNIC_RX_BUF     0x400u      /* RO  0x600 bytes */
#define VNIC_TX_BUF     0xA00u      /* RW  0x600 bytes */
#define VNIC_ID_VALUE   0x434E3349u /* 'I3NC' */
#define VNIC_CTRL_EN    1u
#define VNIC_CTRL_RESET 2u
#define VNIC_ST_LINK    1u
#define VNIC_ST_RX      2u
#define VNIC_ST_TXIDLE  4u
