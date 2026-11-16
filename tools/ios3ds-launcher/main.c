/*
 * ios3ds launcher: a .3dsx for the Homebrew Launcher that boots ios3ds.
 *
 * A .3dsx runs under Horizon and cannot host another kernel, and Luma3DS has
 * no "launch this .firm" call for homebrew (its firmlaunch hook always reloads
 * the file Luma itself booted from). So the launcher swaps files on the SD
 * card and reboots; boot9strap then boots ios3ds.firm directly and the ios3ds
 * monitor's first action is to swap Luma back (boot/arm9/swap.c):
 *
 *   1. create  sdmc:/ios3ds/swap.flg
 *   2. rename  sdmc:/boot.firm          -> sdmc:/ios3ds/luma.firm
 *   3. rename  sdmc:/ios3ds/ios3ds.firm -> sdmc:/boot.firm
 *   4. reboot
 *
 * Only renames (no copies) on the SD card, so a power loss never leaves a
 * half-written boot.firm. Nothing touches NAND. See docs/real-hardware.md.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <3ds.h>
#include <stdio.h>
#include <sys/stat.h>

#define FLAG "sdmc:/ios3ds/swap.flg"
#define LUMA "sdmc:/ios3ds/luma.firm"
#define SELF "sdmc:/ios3ds/ios3ds.firm"
#define BOOT "sdmc:/boot.firm"
#define VERSION "ios3ds launcher 1"

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void reboot(void)
{
    /* full hardware reboot; ptm:sysm first, APT as fallback */
    if (R_SUCCEEDED(ptmSysmInit())) {
        PTMSYSM_RebootAsync(0);
        ptmSysmExit();
    }
    APT_HardwareResetAsync();
}

/* restore Luma by hand (e.g. if ios3ds never got to run) */
static bool restore(void)
{
    if (!exists(LUMA)) return false;
    if (exists(BOOT) && rename(BOOT, SELF) != 0) return false;
    if (rename(LUMA, BOOT) != 0) return false;
    remove(FLAG);
    return true;
}

static bool swap(void)
{
    FILE *f = fopen(FLAG, "w");
    if (!f) return false;
    fputs("swapped by " VERSION "\n", f);
    fclose(f);
    if (rename(BOOT, LUMA) != 0) {
        remove(FLAG);
        return false;
    }
    if (rename(SELF, BOOT) != 0) {
        rename(LUMA, BOOT);                     /* undo step 2 */
        remove(FLAG);
        return false;
    }
    return true;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[1;1H%s\n\n", VERSION);

    bool have_self = exists(SELF), have_boot = exists(BOOT), pending = exists(LUMA);
    if (pending) {
        printf("A previous launch was not undone yet:\n  %s exists.\n\n"
               "Y: restore Luma as boot.firm now\nSTART: exit\n", LUMA);
    } else if (!have_self || !have_boot) {
        printf("Missing %s\n\nCopy build/sd/ from the ios3ds build\n"
               "to the SD card first.\n\nSTART: exit\n", !have_self ? SELF : BOOT);
    } else {
        printf("Boots iOS (ios3ds) on this console.\n\n"
               "The 3DS reboots into ios3ds. Luma stays\n"
               "your boot.firm afterwards: ios3ds puts it\n"
               "back before it does anything else.\n\n"
               "To leave iOS: hold POWER ~10 s.\n\n"
               "A: launch ios3ds    START: exit\n");
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if (pending && (k & KEY_Y)) {
            printf("\n%s\n", restore() ? "Restored. Luma boots next time."
                                       : "Restore FAILED - rename ios3ds/luma.firm\nto boot.firm on a PC.");
            pending = false;
        }
        if (!pending && have_self && have_boot && (k & KEY_A)) {
            if (swap()) {
                printf("\nRebooting into ios3ds...\n");
                gfxFlushBuffers();
                gfxSwapBuffers();
                svcSleepThread(500000000LL);
                reboot();
            } else {
                printf("\nSwap failed, nothing changed.\n");
            }
            have_self = false;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
