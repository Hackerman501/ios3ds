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
 * half-written boot.firm. Nothing writes NAND. See docs/real-hardware.md.
 *
 * Before that, once: if sdmc:/ios3ds/nwm.code (and wifi-fw.bin) is missing,
 * the launcher reads the console's own NWM .code (the Wi-Fi sysmodule, title
 * 0004013000002D02 on NAND) through Horizon's FS, read-only, and writes it
 * there for the ARM9 Wi-Fi loader (boot/arm9/wifi/nwmcode.c). docs/wifi.md §3.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/stat.h>

#define FLAG "sdmc:/ios3ds/swap.flg"
#define LUMA "sdmc:/ios3ds/luma.firm"
#define SELF "sdmc:/ios3ds/ios3ds.firm"
#define BOOT "sdmc:/boot.firm"
#define NWMCODE "sdmc:/ios3ds/nwm.code"
#define NWMTMP  "sdmc:/ios3ds/nwm.code.tmp"
#define WIFIFW  "sdmc:/ios3ds/wifi-fw.bin"
#define VERSION "ios3ds launcher 2"

#define NWM_TID     0x0004013000002D02ULL   /* NWM, same on Old and New 3DS */
#define NWM_MIN     (64u << 10)             /* NWM .code holds 3 firmware images */
#define NWM_MAX     (4u << 20)              /* the ARM9 loader's FCRAM scratch */
#define CHUNK       (64u << 10)

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

/* ---- NWM .code from NAND, read-only --------------------------------------------
 * Archive 0x2345678A ("SaveDataAndContent", title access), 3dbrew
 * "Filesystem services":
 *   archive lowpath (16 bytes): u64 title id, u32 media type, u32 0
 *     -- as libctru romfsMountFromTitle(), FBI listtitles.c, Checkpoint smdh.cpp
 *   file lowpath (20 bytes): u32 0 (high-level NCCH access), u32 0 (TMD content
 *     index), u32 NCCH access type, char name[8]
 *     -- FBI/Checkpoint read exefs:/icon with type 2; type 1 is exefs:/.code,
 *        the access Luma3DS's loader (loader.c codeContentPath) uses.
 * FS returns .code as stored in the ExeFS: BLZ-compressed when the exheader
 * says so (Luma's loader decompresses it itself); nwmcode.c takes both.
 * 3dbrew: fs:USER does not allow type 1, only FSPXI does. So FSUSER is tried
 * first (costs nothing), then FSPXI over the "PxiFS0" session borrowed with
 * Luma3DS's svcControlService (SVC 0xB0), as Checkpoint archive.cpp does.
 * Both are opened FS_OPEN_READ only; the archive cannot be written anyway. */
static const u32 nwm_arch[4] = { (u32)NWM_TID, (u32)(NWM_TID >> 32), MEDIATYPE_NAND, 0 };
static const u32 nwm_file[5] = { 0, 0, 1, 0x646F632E /* ".cod" */, 0x00000065 /* "e\0\0\0" */ };

static Result luma_steal_session(Handle *out, const char *name)
{
    register u32 r0 __asm__("r0") = 0;          /* SERVICEOP_STEAL_CLIENT_SESSION */
    register u32 r1 __asm__("r1") = (u32)out;
    register u32 r2 __asm__("r2") = (u32)name;
    __asm__ volatile("svc 0xB0" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "r12", "memory", "cc");
    return (Result)r0;
}

static bool is_luma(void)
{
    s64 v = 0;                                  /* Luma3DS: version in GetSystemInfo 0x10000 */
    return R_SUCCEEDED(svcGetSystemInfo(&v, 0x10000, 0)) && v != 0;
}

typedef struct {
    bool pxi;
    Handle file, pxs;                           /* FSUSER file / borrowed PxiFS0 session */
    FSPXI_Archive arch;
    FSPXI_File pf;
} nwmsrc_t;

static Result src_open(nwmsrc_t *s, Result *user_err)
{
    FS_Path a = { PATH_BINARY, sizeof(nwm_arch), nwm_arch };
    FS_Path f = { PATH_BINARY, sizeof(nwm_file), nwm_file };
    memset(s, 0, sizeof(*s));
    Result r = *user_err = FSUSER_OpenFileDirectly(&s->file, ARCHIVE_SAVEDATA_AND_CONTENT, a, f, FS_OPEN_READ, 0);
    if (R_SUCCEEDED(r)) return r;
    if (!is_luma()) return r;
    s->pxi = true;
    if (R_FAILED(r = luma_steal_session(&s->pxs, "PxiFS0"))) { s->pxs = 0; return r; }
    if (R_FAILED(r = FSPXI_OpenArchive(s->pxs, &s->arch, ARCHIVE_SAVEDATA_AND_CONTENT, a))) {
        svcCloseHandle(s->pxs); s->pxs = 0; return r;
    }
    if (R_FAILED(r = FSPXI_OpenFile(s->pxs, &s->pf, s->arch, f, FS_OPEN_READ, 0))) {
        FSPXI_CloseArchive(s->pxs, s->arch); svcCloseHandle(s->pxs); s->pxs = 0; return r;
    }
    return 0;
}

static Result src_size(nwmsrc_t *s, u64 *n)
{
    return s->pxi ? FSPXI_GetFileSize(s->pxs, s->pf, n) : FSFILE_GetSize(s->file, n);
}

static Result src_read(nwmsrc_t *s, u32 off, void *buf, u32 len, u32 *got)
{
    return s->pxi ? FSPXI_ReadFile(s->pxs, s->pf, got, off, buf, len)
                  : FSFILE_Read(s->file, got, off, buf, len);
}

static void src_close(nwmsrc_t *s)
{
    if (s->pxi) {
        FSPXI_CloseFile(s->pxs, s->pf);
        FSPXI_CloseArchive(s->pxs, s->arch);
        svcCloseHandle(s->pxs);                 /* our reference only; FS keeps its own */
    } else {
        FSFILE_Close(s->file);
    }
}

static void frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* Returns true when nwm.code is on the card afterwards. Every failure prints
   one red line; nothing is left behind and the launch goes on (Wi-Fi off). */
static bool fetch_nwm_code(void)
{
    if (exists(NWMCODE) || exists(WIFIFW)) return true;
    printf("Wi-Fi firmware: reading NWM .code\nfrom this console (read-only)...\n");
    frame();

    nwmsrc_t s;
    Result ue = 0, r = src_open(&s, &ue);
    if (R_FAILED(r)) {
        if (s.pxi)
            printf("\x1b[31mWi-Fi fw: cannot open NWM .code\n(FS %08lX, FSPXI %08lX): Wi-Fi off\x1b[0m\n\n",
                   (unsigned long)ue, (unsigned long)r);
        else
            printf("\x1b[31mWi-Fi fw: cannot open NWM .code\n(FS %08lX, no Luma3DS for FSPXI): Wi-Fi off\x1b[0m\n\n",
                   (unsigned long)ue);
        return false;
    }
    u64 sz64 = 0;
    u8 *buf = NULL;
    const char *why = NULL;
    u32 got = 0;
    if (R_FAILED(r = src_size(&s, &sz64))) why = "size query failed";
    else if (sz64 < NWM_MIN || sz64 > NWM_MAX) why = "unexpected size";
    else if (!(buf = memalign(0x1000, (u32)sz64))) why = "out of memory";
    else {
        u32 sz = (u32)sz64;
        while (got < sz) {
            u32 n = 0, want = sz - got < CHUNK ? sz - got : CHUNK;
            if (R_FAILED(r = src_read(&s, got, buf + got, want, &n)) || n == 0) {
                why = "read failed";
                break;
            }
            got += n;
            printf("\r  %lu / %lu KiB via %s", (unsigned long)(got >> 10), (unsigned long)(sz >> 10),
                   s.pxi ? "FSPXI" : "FS");
            frame();
        }
        printf("\n");
    }
    src_close(&s);

    if (!why) {
        FILE *f = fopen(NWMTMP, "wb");
        if (!f) why = "cannot create " NWMTMP;
        else {
            bool ok = fwrite(buf, 1, got, f) == got;
            ok = fclose(f) == 0 && ok;
            if (!ok) why = "SD write failed";
            else if (rename(NWMTMP, NWMCODE) != 0) why = "rename failed";
        }
        if (why) remove(NWMTMP);
    }
    if (buf) {
        memset(buf, 0, (u32)sz64);              /* Nintendo's code stays only on the card */
        free(buf);
    }
    if (why) {
        printf("\x1b[31mWi-Fi fw: %s (%08lX, %llu bytes):\nWi-Fi off\x1b[0m\n\n", why,
               (unsigned long)r, (unsigned long long)sz64);
        return false;
    }
    printf("Wrote %s\n(%lu bytes). Keep it private.\n\n", NWMCODE + 5, (unsigned long)got);
    return true;
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
        fetch_nwm_code();
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
