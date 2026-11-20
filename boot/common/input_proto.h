/*
 * ios3ds input mailbox: 3DS buttons + touch panel, ARM9 -> XNU shim -> iOS daemon.
 * Shared by arm9/tsc.c (producer), shim/input.c (kernel side) and
 * daemon/input.c (userland consumer, which gets a byte copy through
 * sysctl kern.ios3ds_input). See docs/input-design.md.
 *
 * NOT wired into any build yet. When it is, these definitions belong in
 * boot/common/ios3ds.h next to blkmbox_t.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* Inside the shim region (SHIM_PA 0x20840000 / SHIM_VA 0x80840000):
   0x6000 blkmbox_t (128 bytes), 0x6200 inmbox_t (512 bytes), 0x6400 mcubox_t
   (256 bytes, mcu_proto.h), 0x8000 bounce. */
#define SHIM_INPUT_OFF   0x6200u
#define INPUT_MAGIC      0x54504E49u    /* 'INPT' */
#define INPUT_VERSION    1u
#define INPUT_RING       30u            /* samples kept; only changes are pushed */

/* insample_t.buttons: pressed = 1 (the ARM9 inverts HID_PAD). Bits 0-11 are
   the HID_PAD bits (A B SELECT START RIGHT LEFT UP DOWN R L X Y). */
#define INBTN_PAD_MASK   0x0FFFu
#define INBTN_HOME       (1u << 12)     /* MCU IRQ bits 2/3 (boot/arm9/mcu.c, docs/mcu.md) */
#define INBTN_POWER      (1u << 13)     /* MCU IRQ bits 0/1 (boot/arm9/mcu.c) */
#define INBTN_ZL         (1u << 14)     /* New 3DS (boot/arm9/exthid.c, docs/n3ds.md) */
#define INBTN_ZR         (1u << 15)     /* New 3DS */

/* inmbox_t.ext: the New 3DS C-stick, updated on every ARM9 poll (a level,
   not an event: the daemon reads the latest value). 0 = no such chip. */
#define INEXT_X(e)       ((int)(int8_t)((e) & 0xFFu))          /* right positive, about +-0x7x */
#define INEXT_Y(e)       ((int)(int8_t)(((e) >> 8) & 0xFFu))   /* up positive */
#define INEXT_VALID      (1u << 24)
#define INEXT_ZL         (1u << 25)
#define INEXT_ZR         (1u << 26)

#define INFLAG_PEN       (1u << 0)      /* stylus/finger on the panel; tx/ty valid */
#define INFLAG_CPAD      (1u << 1)      /* cx/cy valid */

typedef struct {
    uint16_t buttons;   /* INBTN_* */
    uint16_t flags;     /* INFLAG_* */
    uint16_t tx, ty;    /* raw 12-bit ADC: tx 0..4095 = left..right of the bottom
                           screen (320 px), ty 0..4095 = top..bottom (240 px) */
    int16_t  cx, cy;    /* circle pad, raw - 2048 (x already un-inverted) */
    uint32_t tick;      /* ARM9 poll counter when the sample was taken */
} insample_t;

/* Single writer for every cache line: the ARM9 (FCRAM is uncached there).
   The ARM11 only reads, after invalidating the lines. `head` is written last. */
typedef struct {
    uint32_t magic;         /* INPUT_MAGIC once the ARM9 polls input */
    uint32_t version;       /* INPUT_VERSION */
    uint32_t head;          /* samples ever pushed; s[(head-1) % INPUT_RING] is the newest */
    uint32_t tsc_ok;        /* 1 = CODEC answered and the touch ADC is running */
    uint32_t polls;         /* total ARM9 polls (liveness) */
    uint32_t ext;           /* New 3DS C-stick (INEXT_*), 0 on other models */
    uint32_t _r[2];
    insample_t s[INPUT_RING];
} inmbox_t;

_Static_assert(sizeof(insample_t) == 16, "insample layout");
_Static_assert(sizeof(inmbox_t) == 512, "inmbox layout");

#define INPUT_SYSCTL_NAME "kern.ios3ds_input"
