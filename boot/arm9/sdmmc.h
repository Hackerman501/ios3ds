/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "ios3ds.h"

#define SDMMC_ERR_NOCARD  5
#define SDMMC_ERR_TIMEOUT 10
#define SDMMC_ERR_ACMD41  11
#define SDMMC_ERR_IO      12

int sdmmc_sd_init(void);
int sdmmc_sd_read(u32 sector, u32 count, u8 *out);
int sdmmc_sd_write(u32 sector, u32 count, const u8 *in);
u32 sdmmc_sd_sectors(void);
bool sdmmc_sd_is_sdhc(void);
