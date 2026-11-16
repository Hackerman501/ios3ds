/*
 * picasim: a small software model of the PICA200 as ios3ds uses it.
 *
 * It models the GX register block at PA 0x10400000 (PSC memory fill, PPF
 * display transfer / texture copy, the P3D command-list processor) and
 * renders with a software rasterizer. The same file is compiled into
 *   - the QEMU machine (emu/qemu/hw/arm/nintendo3ds.c), so the iOS guest's
 *     PICA back end has a GPU to talk to, and
 *   - the host tests (tools/ios3ds-pica/tests, tools/ios3ds-gl host-test).
 *
 * Register semantics follow the Citra/Azahar software renderer (GPL-2.0+,
 * src/video_core); nothing was copied, see docs/pica200.md for the mapping
 * and for what is deliberately not modelled.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PICASIM_H
#define PICASIM_H
#include <stdint.h>

typedef struct PicaSimHost {
    /* Host pointer for the guest-physical range [pa, pa+len), or NULL when
       the GPU cannot reach it. write != 0: the range will be modified. */
    uint8_t *(*mem)(void *opaque, uint32_t pa, uint32_t len, int write);
    /* An interrupt line fired: GIC ids 40 (PSC0), 41 (PSC1), 44 (PPF), 45 (P3D). */
    void (*irq)(void *opaque, int gic_id);
    /* One line of diagnostics (unsupported register use, bad addresses). */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} PicaSimHost;

typedef struct PicaSimStats {
    uint32_t lists, commands, draws, vertices, triangles, pixels;
    uint32_t fills, transfers, texcopies;
    uint32_t errors;            /* things the model refused or does not implement */
} PicaSimStats;

typedef struct PicaSim {
    PicaSimHost host;
    uint32_t gx[0x1000 / 4];        /* GX MMIO words 0x000-0xFFF */
    uint32_t reg[0x300];            /* P3D internal registers */
    uint32_t code[512], opdesc[128];
    float uni[96][4];
    float fixed[12][4];
    uint32_t fifo[4];               /* float uniform / fixed attribute input words */
    int fifo_n;
    int uni_index, uni_f32;
    int fixed_index;
    uint32_t code_off, opdesc_off;
    PicaSimStats st;
    int logged;                     /* diagnostics printed so far (rate limit) */
} PicaSim;

void picasim_init(PicaSim *s, const PicaSimHost *host);
/* MMIO access; off is the byte offset from 0x10400000 (0 .. 0x1FFF). */
uint32_t picasim_read(PicaSim *s, uint32_t off);
void picasim_write(PicaSim *s, uint32_t off, uint32_t val);
#endif
