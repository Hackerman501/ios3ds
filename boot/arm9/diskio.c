/* FatFs glue for the SD card. SPDX-License-Identifier: GPL-3.0-or-later */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "sdmmc.h"

static bool inited;

DSTATUS disk_status(BYTE pdrv)
{
    return (pdrv == 0 && inited) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;
    }
    if (!inited) {
        inited = sdmmc_sd_init() == 0;
    }
    return inited ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    return sdmmc_sd_read(sector, count, buff) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    return sdmmc_sd_write(sector, count, buff) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sdmmc_sd_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    }
    return RES_PARERR;
}
