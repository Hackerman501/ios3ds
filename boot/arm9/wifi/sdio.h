/*
 * SDIO host for the 3DS Wi-Fi slot: TMIO controller 2 at 0x10122000, driven
 * from the ARM9 by polling (its interrupt, GIC 64/65, only exists on the ARM11).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ios3ds.h"

#define SDIO_BLOCK 128u         /* function 1 block size (same as NWM / dsiwifi) */

enum {
    SDIO_OK = 0,
    SDIO_ERR_TIMEOUT = -1,      /* controller never finished the command */
    SDIO_ERR_CARD = -2,         /* error bits in STAT1 (see sdio_last_status) */
    SDIO_ERR_NOCARD = -3,       /* no answer to CMD5: chip unpowered / held in reset */
    SDIO_ERR_ARG = -4,
};

/* Power the port, reset the controller, enumerate the card (CMD5/CMD3/CMD7),
   switch to 4-bit, set the block size and enable function 1. */
int sdio_init(void);
/* CMD52. sdio_r8 returns the byte (0..255) or a negative SDIO_ERR_*. */
int sdio_r8(u32 func, u32 addr);
int sdio_w8(u32 func, u32 addr, u8 val);
/* CMD53, block mode, fixed address. len must be a multiple of SDIO_BLOCK and
   buf 4-byte aligned. */
int sdio_read_blocks(u32 func, u32 addr, u8 *buf, u32 len);
int sdio_write_blocks(u32 func, u32 addr, const u8 *buf, u32 len);
/* STAT1 | STAT0 << 16 of the last command, for the log. */
u32 sdio_last_status(void);
