/*
 * Undo the boot.firm swap made by the ios3ds launcher (.3dsx).
 *
 * The launcher (tools/ios3ds-launcher) does, on the SD card only:
 *   1. create  ios3ds/swap.flg
 *   2. rename  boot.firm          -> ios3ds/luma.firm     (Luma3DS)
 *   3. rename  ios3ds/ios3ds.firm -> boot.firm            (this payload)
 *   4. reboot  -> boot9strap boots us directly
 * and the very first thing we do after mounting the SD card is the reverse, so
 * the next power-on is Luma again. Every interrupted state still boots:
 * between the two renames there is no sd:/boot.firm and boot9strap falls back
 * to ctrnand:/boot.firm (installed by the 3ds.hacks.guide); after the swap,
 * this payload boots and restores. Nothing here touches NAND.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "swap.h"
#include "log.h"
#include "fatfs/ff.h"

#define FLAG  "0:/ios3ds/swap.flg"
#define LUMA  "0:/ios3ds/luma.firm"
#define SELF  "0:/ios3ds/ios3ds.firm"
#define BOOT  "0:/boot.firm"

static bool exists(const char *p)
{
    FILINFO fi;
    return f_stat(p, &fi) == FR_OK;
}

void firm_swap_restore(void)
{
    if (!exists(FLAG)) return;
    logp("launcher swap: restoring Luma as sd:/boot.firm\n");
    if (!exists(LUMA)) {
        /* already restored (or never swapped): just drop the flag */
        log_warn("launcher swap: no %s, nothing to restore\n", LUMA);
        f_unlink(FLAG);
        return;
    }
    FRESULT r = FR_OK;
    if (exists(BOOT)) {
        if (exists(SELF)) r = f_unlink(BOOT);   /* boot.firm is a copy of us */
        else r = f_rename(BOOT, SELF);
    }
    if (r == FR_OK) r = f_rename(LUMA, BOOT);
    if (r != FR_OK) {
        log_err("launcher swap: restore failed (FRESULT %d); fix by hand: "
                "ios3ds/luma.firm -> boot.firm\n", r);
        return;                                 /* keep the flag: retry next boot */
    }
    f_unlink(FLAG);
    log_ok("launcher swap: restored, next boot is Luma\n");
}
