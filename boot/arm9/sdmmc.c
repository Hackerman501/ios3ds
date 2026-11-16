/*
 * SD card driver for the 3DS TMIO controller (ARM9, polling, SD slot only).
 *
 * Adapted from sdmmc.c by Normmatt (as shipped in linux-3ds/firm_linux_loader
 * and arm9linuxfw), which is dual-licensed MPL-2.0 / GPL-2.0-or-later:
 *
 *   Copyright (c) 2014-2015, Normmatt
 *   This file is free software: you may copy, redistribute and/or modify it
 *   under the terms of the GNU General Public License as published by the
 *   Free Software Foundation, either version 2 of the License, or (at your
 *   option) any later version.
 *
 * ios3ds changes: removed the eMMC/NAND paths entirely (we never touch NAND,
 * not even its init sequence), returned error codes, no libc dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sdmmc.h"

#define SDMMC_BASE 0x10006000u

#define REG_SDCMD        0x00
#define REG_SDPORTSEL    0x02
#define REG_SDCMDARG0    0x04
#define REG_SDCMDARG1    0x06
#define REG_SDSTOP       0x08
#define REG_SDBLKCOUNT   0x0a
#define REG_SDRESP0      0x0c
#define REG_SDSTATUS0    0x1c
#define REG_SDSTATUS1    0x1e
#define REG_SDIRMASK0    0x20
#define REG_SDIRMASK1    0x22
#define REG_SDCLKCTL     0x24
#define REG_SDBLKLEN     0x26
#define REG_SDOPT        0x28
#define REG_DATACTL      0xd8
#define REG_SDRESET      0xe0
#define REG_DATACTL32    0x100
#define REG_SDBLKLEN32   0x104
#define REG_SDBLKCOUNT32 0x108
#define REG_SDFIFO32     0x10C

#define TMIO_STAT0_CMDRESPEND  0x0001
#define TMIO_STAT0_DATAEND     0x0004
#define TMIO_STAT0_SIGSTATE    0x0020
#define TMIO_STAT1_CMD_BUSY    0x4000
#define TMIO_STAT1_RXRDY       0x0100
#define TMIO_STAT1_TXRQ        0x0200
#define TMIO_MASK_ALL          0x837f031du
#define TMIO_MASK_GW           (0x8000 | 0x0040 | 0x0020 | 0x0010 | 0x0008 | 0x0004 | 0x0002 | 0x0001)

typedef struct {
    u8 *rData;
    const u8 *tData;
    u32 size;
    u32 error;
    u16 stat0, stat1;
    u32 ret[4];
    u32 initarg;
    u32 isSDHC;
    u32 clk;
    u32 SDOPT;
    u32 devicenumber;
    u32 total_size;
} mmcdevice;

static mmcdevice handelSD;

static inline u16 rd16(u16 reg) { return *(vu16 *)(SDMMC_BASE + reg); }
static inline void wr16(u16 reg, u16 v) { *(vu16 *)(SDMMC_BASE + reg) = v; }
static inline u32 rd32(u16 reg) { return *(vu32 *)(SDMMC_BASE + reg); }
static inline void wr32(u16 reg, u32 v) { *(vu32 *)(SDMMC_BASE + reg) = v; }
static inline void mask16(u16 reg, u16 clear, u16 set)
{
    u16 v = rd16(reg);
    v &= ~clear;
    v |= set;
    wr16(reg, v);
}

void waitcycles(u32 n);

static void setckl(u32 data)
{
    mask16(REG_SDCLKCTL, 0x100, 0);
    mask16(REG_SDCLKCTL, 0x2FF, data & 0x2FF);
    mask16(REG_SDCLKCTL, 0x0, 0x100);
}

static void inittarget(mmcdevice *ctx)
{
    mask16(REG_SDPORTSEL, 0x3, (u16)ctx->devicenumber);
    setckl(ctx->clk);
    if (ctx->SDOPT == 0) {
        mask16(REG_SDOPT, 0, 0x8000);
    } else {
        mask16(REG_SDOPT, 0x8000, 0);
    }
}

/* returns false on timeout (controller wedged) */
static bool __attribute__((noinline)) send_command(mmcdevice *ctx, u32 cmd, u32 args)
{
    const u32 getSDRESP = (cmd << 15) >> 31;
    u16 flags = (cmd << 15) >> 31;
    const int readdata = cmd & 0x20000;
    const int writedata = cmd & 0x40000;
    u32 guard = 0;

    if (readdata || writedata) {
        flags |= TMIO_STAT0_DATAEND;
    }
    ctx->error = 0;
    while (rd16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY) {
        if (++guard > 50000000u) return false;
    }
    wr16(REG_SDIRMASK0, 0);
    wr16(REG_SDIRMASK1, 0);
    wr16(REG_SDSTATUS0, 0);
    wr16(REG_SDSTATUS1, 0);
    mask16(REG_DATACTL32, 0x1800, 0);
    wr16(REG_SDCMDARG0, args & 0xFFFF);
    wr16(REG_SDCMDARG1, args >> 16);
    wr16(REG_SDCMD, cmd & 0xFFFF);

    u32 size = ctx->size;
    u8 *rDataPtr = ctx->rData;
    const u8 *tDataPtr = ctx->tData;
    u16 status0 = 0;
    guard = 0;

    for (;;) {
        vu16 status1 = rd16(REG_SDSTATUS1);
        vu16 ctl32 = rd16(REG_DATACTL32);
        if (++guard > 200000000u) return false;

        if ((ctl32 & 0x100) && readdata && rDataPtr) {
            mask16(REG_SDSTATUS1, TMIO_STAT1_RXRDY, 0);
            if (size > 0x1FF) {
                for (int i = 0; i < 0x200; i += 4) {
                    u32 data = rd32(REG_SDFIFO32);
                    *rDataPtr++ = data;
                    *rDataPtr++ = data >> 8;
                    *rDataPtr++ = data >> 16;
                    *rDataPtr++ = data >> 24;
                }
                size -= 0x200;
            }
            mask16(REG_DATACTL32, 0x800, 0);
        }
        if (!(ctl32 & 0x200) && writedata && tDataPtr) {
            mask16(REG_SDSTATUS1, TMIO_STAT1_TXRQ, 0);
            if (size > 0x1FF) {
                for (int i = 0; i < 0x200; i += 4) {
                    u32 data = *tDataPtr++;
                    data |= (u32)*tDataPtr++ << 8;
                    data |= (u32)*tDataPtr++ << 16;
                    data |= (u32)*tDataPtr++ << 24;
                    wr32(REG_SDFIFO32, data);
                }
                size -= 0x200;
            }
            mask16(REG_DATACTL32, 0x1000, 0);
        }
        if (status1 & TMIO_MASK_GW) {
            ctx->error |= 4;
            break;
        }
        if (!(status1 & TMIO_STAT1_CMD_BUSY)) {
            status0 = rd16(REG_SDSTATUS0);
            if (rd16(REG_SDSTATUS0) & TMIO_STAT0_CMDRESPEND) {
                ctx->error |= 0x1;
            }
            if (status0 & TMIO_STAT0_DATAEND) {
                ctx->error |= 0x2;
            }
            if ((status0 & flags) == flags) {
                break;
            }
        }
    }
    ctx->stat0 = rd16(REG_SDSTATUS0);
    ctx->stat1 = rd16(REG_SDSTATUS1);
    wr16(REG_SDSTATUS0, 0);
    wr16(REG_SDSTATUS1, 0);

    if (getSDRESP) {
        ctx->ret[0] = (u32)(rd16(REG_SDRESP0 + 0) | (rd16(REG_SDRESP0 + 2) << 16));
        ctx->ret[1] = (u32)(rd16(REG_SDRESP0 + 4) | (rd16(REG_SDRESP0 + 6) << 16));
        ctx->ret[2] = (u32)(rd16(REG_SDRESP0 + 8) | (rd16(REG_SDRESP0 + 10) << 16));
        ctx->ret[3] = (u32)(rd16(REG_SDRESP0 + 12) | (rd16(REG_SDRESP0 + 14) << 16));
    }
    return true;
}

static u32 calcSDSize(const u8 *csd, int type)
{
    u32 result = 0;
    if (type == -1) {
        type = csd[14] >> 6;
    }
    switch (type) {
    case 0: {
        u32 block_len = csd[9] & 0xf;
        block_len = 1u << block_len;
        u32 mult = (u32)((csd[4] >> 7) | ((csd[5] & 3) << 1));
        mult = 1u << (mult + 2);
        result = csd[8] & 3;
        result = (result << 8) | csd[7];
        result = (result << 2) | (csd[6] >> 6);
        result = (result + 1) * mult * block_len / 512;
        break;
    }
    case 1:
        result = csd[7] & 0x3f;
        result = (result << 8) | csd[6];
        result = (result << 8) | csd[5];
        result = (result + 1) * 1024;
        break;
    }
    return result;
}

static void init_controller(void)
{
    *(vu16 *)0x10006100 &= 0xF7FFu;
    *(vu16 *)0x10006100 &= 0xEFFFu;
    *(vu16 *)0x10006100 |= 0x402u;
    *(vu16 *)0x100060D8 = (*(vu16 *)0x100060D8 & 0xFFDD) | 2;
    *(vu16 *)0x10006100 &= 0xFFFFu;
    *(vu16 *)0x100060D8 &= 0xFFDFu;
    *(vu16 *)0x10006104 = 512;
    *(vu16 *)0x10006108 = 1;
    *(vu16 *)0x100060E0 &= 0xFFFEu;
    *(vu16 *)0x100060E0 |= 1u;
    *(vu16 *)0x10006020 |= TMIO_MASK_ALL;
    *(vu16 *)0x10006022 |= TMIO_MASK_ALL >> 16;
    *(vu16 *)0x100060FC |= 0xDBu;
    *(vu16 *)0x100060FE |= 0xDBu;
    *(vu16 *)0x10006002 &= 0xFFFCu;
    *(vu16 *)0x10006024 = 0x20;
    *(vu16 *)0x10006028 = 0x40EE;
    *(vu16 *)0x10006002 &= 0xFFFCu;
    *(vu16 *)0x10006026 = 512;
    *(vu16 *)0x10006008 = 0;
}

int sdmmc_sd_init(void)
{
    init_controller();

    handelSD.isSDHC = 0;
    handelSD.SDOPT = 0;
    handelSD.initarg = 0;
    handelSD.clk = 0x80;
    handelSD.devicenumber = 0;
    handelSD.rData = NULL;
    handelSD.tData = NULL;
    handelSD.size = 0;
    inittarget(&handelSD);
    waitcycles(1u << 22);

    if (!(rd16(REG_SDSTATUS0) & TMIO_STAT0_SIGSTATE)) {
        return SDMMC_ERR_NOCARD;
    }
    if (!send_command(&handelSD, 0, 0)) return SDMMC_ERR_TIMEOUT;
    send_command(&handelSD, 0x10408, 0x1AA);
    u32 temp = (handelSD.error & 0x1) << 0x1E;
    u32 temp2 = 0;
    u32 tries = 0;
    do {
        do {
            send_command(&handelSD, 0x10437, handelSD.initarg << 0x10);
            send_command(&handelSD, 0x10769, 0x00FF8000 | temp);
            temp2 = 1;
            if (++tries > 100000) return SDMMC_ERR_ACMD41;
        } while (!(handelSD.error & 1));
    } while ((handelSD.ret[0] & 0x80000000) == 0);

    if (!((handelSD.ret[0] >> 30) & 1) || !temp) {
        temp2 = 0;
    }
    handelSD.isSDHC = temp2;

    send_command(&handelSD, 0x10602, 0);
    if (handelSD.error & 0x4) return -1;
    send_command(&handelSD, 0x10403, 0);
    if (handelSD.error & 0x4) return -2;
    handelSD.initarg = handelSD.ret[0] >> 0x10;
    send_command(&handelSD, 0x10609, handelSD.initarg << 0x10);
    if (handelSD.error & 0x4) return -3;
    handelSD.total_size = calcSDSize((const u8 *)&handelSD.ret[0], -1);
    handelSD.clk = 1;
    setckl(1);
    send_command(&handelSD, 0x10507, handelSD.initarg << 0x10);
    if (handelSD.error & 0x4) return -4;
    send_command(&handelSD, 0x10437, handelSD.initarg << 0x10);
    if (handelSD.error & 0x4) return -5;
    handelSD.SDOPT = 1;
    send_command(&handelSD, 0x10446, 0x2);
    if (handelSD.error & 0x4) return -6;
    send_command(&handelSD, 0x1040D, handelSD.initarg << 0x10);
    if (handelSD.error & 0x4) return -7;
    send_command(&handelSD, 0x10410, 0x200);
    if (handelSD.error & 0x4) return -8;
    handelSD.clk |= 0x200;
    return 0;
}

u32 sdmmc_sd_sectors(void) { return handelSD.total_size; }
bool sdmmc_sd_is_sdhc(void) { return handelSD.isSDHC; }

int sdmmc_sd_read(u32 sector_no, u32 numsectors, u8 *out)
{
    if (handelSD.isSDHC == 0) sector_no <<= 9;
    inittarget(&handelSD);
    wr16(REG_SDSTOP, 0x100);
    wr16(REG_SDBLKCOUNT32, numsectors);
    wr16(REG_SDBLKLEN32, 0x200);
    wr16(REG_SDBLKCOUNT, numsectors);
    handelSD.rData = out;
    handelSD.tData = NULL;
    handelSD.size = numsectors << 9;
    if (!send_command(&handelSD, 0x33C12, sector_no)) return SDMMC_ERR_TIMEOUT;
    handelSD.rData = NULL;
    return (handelSD.error & 4) ? SDMMC_ERR_IO : 0;
}

int sdmmc_sd_write(u32 sector_no, u32 numsectors, const u8 *in)
{
    if (handelSD.isSDHC == 0) sector_no <<= 9;
    inittarget(&handelSD);
    wr16(REG_SDSTOP, 0x100);
    wr16(REG_SDBLKCOUNT32, numsectors);
    wr16(REG_SDBLKLEN32, 0x200);
    wr16(REG_SDBLKCOUNT, numsectors);
    handelSD.tData = in;
    handelSD.rData = NULL;
    handelSD.size = numsectors << 9;
    if (!send_command(&handelSD, 0x52C19, sector_no)) return SDMMC_ERR_TIMEOUT;
    handelSD.tData = NULL;
    return (handelSD.error & 4) ? SDMMC_ERR_IO : 0;
}
