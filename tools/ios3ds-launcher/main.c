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
#include <stdarg.h>
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
#define FWSTATUS "sdmc:/ios3ds/wififw-status.txt"  /* last attempt, read into log.txt by the ARM9 */
#define WIFIFW  "sdmc:/ios3ds/wifi-fw.bin"
#define VERSION "ios3ds launcher 3"

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
static u32 nwm_arch[4];                /* {title id low, high, MEDIATYPE_NAND, 0}: set per title */
static const u32 nwm_file[5] = { 0, 0, 1, 0x646F632E /* ".cod" */, 0x00000065 /* "e\0\0\0" */ };
static const u32 nwm_file2[5] = { 0, 0, 2, 0x646F632E, 0x00000065 };
/* Luma3DS's loader (sysmodules/loader/source/loader.c): archive 0x2345678E
   (SaveDataAndContent2) with FS_ProgramInfo {u64 programId, u8 mediaType,
   pad} as the archive path and the 12-byte file path {u32 1, ".code"}.
   Hardware (issue #5, New 2DS XL): the 0x2345678A layout above gave
   E0E046BE (FS InvalidPath) from both FS and FSPXI. */
#define ARCHIVE_SAVEDATA_AND_CONTENT2_ID 0x2345678E
#define nwm_arch2 nwm_arch              /* FS_ProgramInfo has the same 16-byte layout */
static const u32 nwm_file3[3] = { 1, 0x646F632E, 0x00000065 };

typedef struct { u32 archive; const void *ap; u32 al; const void *fp; u32 fl; const char *name; } nwmpath_t;
static const nwmpath_t nwm_paths[3] = {
    { ARCHIVE_SAVEDATA_AND_CONTENT2_ID, nwm_arch2, sizeof(nwm_arch2), nwm_file3, sizeof(nwm_file3), "678E/1" },
    { ARCHIVE_SAVEDATA_AND_CONTENT, nwm_arch, sizeof(nwm_arch), nwm_file, sizeof(nwm_file), "678A/1" },
    { ARCHIVE_SAVEDATA_AND_CONTENT, nwm_arch, sizeof(nwm_arch), nwm_file2, sizeof(nwm_file2), "678A/2" },
};
static char nwm_tried[160];             /* "678E/1 FS xxxxxxxx PXI xxxxxxxx; ..." for the status file */

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

static Result src_open_one(nwmsrc_t *s, const nwmpath_t *p, Result *user_err)
{
    FS_Path a = { PATH_BINARY, p->al, p->ap };
    FS_Path f = { PATH_BINARY, p->fl, p->fp };
    memset(s, 0, sizeof(*s));
    Result r = *user_err = FSUSER_OpenFileDirectly(&s->file, (FS_ArchiveID)p->archive, a, f, FS_OPEN_READ, 0);
    if (R_SUCCEEDED(r)) return r;
    if (!is_luma()) return r;
    s->pxi = true;
    if (R_FAILED(r = luma_steal_session(&s->pxs, "PxiFS0"))) { s->pxs = 0; return r; }
    if (R_FAILED(r = FSPXI_OpenArchive(s->pxs, &s->arch, (FS_ArchiveID)p->archive, a))) {
        svcCloseHandle(s->pxs); s->pxs = 0; return r;
    }
    if (R_FAILED(r = FSPXI_OpenFile(s->pxs, &s->pf, s->arch, f, FS_OPEN_READ, 0))) {
        FSPXI_CloseArchive(s->pxs, s->arch); svcCloseHandle(s->pxs); s->pxs = 0; return r;
    }
    return 0;
}

/* try each known path layout (FS first, then FSPXI) until one opens */
static Result src_open(nwmsrc_t *s, Result *user_err)
{
    Result r = 0;
    nwm_tried[0] = 0;
    for (unsigned i = 0; i < sizeof(nwm_paths) / sizeof(nwm_paths[0]); i++) {
        /* 0x2345678E only under Luma3DS (always there on a console): Azahar
           2126.2 crashes on that archive id instead of returning an error */
        if (nwm_paths[i].archive == ARCHIVE_SAVEDATA_AND_CONTENT2_ID && !is_luma()) continue;
        r = src_open_one(s, &nwm_paths[i], user_err);
        size_t n = strlen(nwm_tried);
        snprintf(nwm_tried + n, sizeof(nwm_tried) - n, "%s%s FS %08lX%s", n ? "; " : "", nwm_paths[i].name,
                 (unsigned long)*user_err, R_SUCCEEDED(r) ? " ok" : "");
        if (R_SUCCEEDED(r)) return r;
        n = strlen(nwm_tried);
        if (s->pxi) snprintf(nwm_tried + n, sizeof(nwm_tried) - n, " PXI %08lX", (unsigned long)r);
    }
    return r;
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
/* one line for the ARM9 to put into log.txt (issue #5: Wi-Fi greyed out, and
   log.txt could not say why the launcher's copy did not happen) */
static const char *job_status_path;     /* status file of the job running now */

static void fw_status(const char *fmt, ...)
{
    FILE *f = fopen(job_status_path ? job_status_path : FWSTATUS, "w");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%s: ", VERSION);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* A system title whose ExeFS .code the launcher copies to the SD card, read-only:
   NWM (the Wi-Fi firmware source, read by the ARM9 at every boot) and the CAMERA
   sysmodule (its camera start-up sequence is not documented anywhere;
   docs/camera.md). */
typedef struct {
    const char *what;           /* on-screen name */
    u64 tid[2];                 /* tried in order (0 = none) */
    const char *dst, *tmp, *status, *alt;   /* alt: skip if this file exists too */
    u32 min, max;
} codejob_t;

static const codejob_t job_nwm = {
    "Wi-Fi firmware (NWM .code)", { NWM_TID, 0 }, NWMCODE, NWMTMP, FWSTATUS, WIFIFW, NWM_MIN, NWM_MAX,
};
static const codejob_t job_cam_old = {
    "camera module (CAMERA .code)", { 0x0004013000001602ULL, 0 },
    "sdmc:/ios3ds/cam.code", "sdmc:/ios3ds/cam.code.tmp", "sdmc:/ios3ds/camcode-status.txt", NULL,
    16u << 10, 4u << 20,
};
static const codejob_t job_cam_new = {   /* New 3DS: its own CAMERA variant first */
    "camera module (CAMERA .code)", { 0x0004013020001602ULL, 0x0004013000001602ULL },
    "sdmc:/ios3ds/cam.code", "sdmc:/ios3ds/cam.code.tmp", "sdmc:/ios3ds/camcode-status.txt", NULL,
    16u << 10, 4u << 20,
};

/* Returns true when the file is on the card afterwards. Every failure prints
   one red line and leaves nothing behind. */
static bool fetch_code(const codejob_t *j)
{
    if (exists(j->dst) || (j->alt && exists(j->alt))) return true;
    job_status_path = j->status;
    printf("%s: reading it\nfrom this console (read-only)...\n", j->what);
    frame();

    nwmsrc_t s;
    Result ue = 0, r = -1;
    u64 tid = 0;
    char all[320] = "";
    for (int t = 0; t < 2 && j->tid[t]; t++) {
        tid = j->tid[t];
        nwm_arch[0] = (u32)tid; nwm_arch[1] = (u32)(tid >> 32); nwm_arch[2] = MEDIATYPE_NAND; nwm_arch[3] = 0;
        r = src_open(&s, &ue);
        size_t n = strlen(all);
        snprintf(all + n, sizeof(all) - n, "%s%08lx%08lx: %s", n ? " | " : "",
                 (unsigned long)(tid >> 32), (unsigned long)tid, nwm_tried);
        if (R_SUCCEEDED(r)) break;
    }
    if (R_FAILED(r)) {
        printf("\x1b[31m%s: cannot open it:\n%s\x1b[0m\n\n", j->what, all);
        fw_status("cannot open %s: %s", j->what, all);
        return false;
    }
    u64 sz64 = 0;
    u8 *buf = NULL;
    const char *why = NULL;
    u32 got = 0;
    if (R_FAILED(r = src_size(&s, &sz64))) why = "size query failed";
    else if (sz64 < j->min || sz64 > j->max) why = "unexpected size";
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
        FILE *f = fopen(j->tmp, "wb");
        if (!f) why = "cannot create the file";
        else {
            bool ok = fwrite(buf, 1, got, f) == got;
            ok = fclose(f) == 0 && ok;
            if (!ok) why = "SD write failed";
            else if (rename(j->tmp, j->dst) != 0) why = "rename failed";
        }
        if (why) remove(j->tmp);
    }
    if (buf) {
        memset(buf, 0, (u32)sz64);              /* Nintendo's code stays only on the card */
        free(buf);
    }
    if (why) {
        printf("\x1b[31m%s: %s (%08lX, %llu bytes)\x1b[0m\n\n", j->what, why,
               (unsigned long)r, (unsigned long long)sz64);
        fw_status("%s: %s (%08lX, %llu bytes)", j->what, why, (unsigned long)r, (unsigned long long)sz64);
        return false;
    }
    fw_status("copied %s from %08lx%08lx (%lu bytes, via %s; %s)", j->what, (unsigned long)(tid >> 32),
              (unsigned long)tid, (unsigned long)got, s.pxi ? "FSPXI" : "FS", nwm_tried);
    printf("Wrote %s\n(%lu bytes). Keep it private.\n\n", j->dst + 5, (unsigned long)got);
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
        /* Clear stale status first: the ARM9 log can then prove that this exact
           Homebrew Launcher binary ran before the reboot, even if the copy is
           skipped because the file is already present. */
        remove(FWSTATUS);
        fw_status("launcher entered from Homebrew Launcher; NWM .code %s, wifi-fw.bin %s",
                  exists(NWMCODE) ? "present" : "missing", exists(WIFIFW) ? "present" : "missing");
        fetch_code(&job_nwm);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        fetch_code(n3ds ? &job_cam_new : &job_cam_old);
        printf("Boots iOS (ios3ds) on this console.\n\n"
               "The 3DS reboots into ios3ds. Luma stays\n"
               "your boot.firm afterwards: ios3ds puts it\n"
               "back before it does anything else.\n\n"
               "To leave iOS: hold POWER for about 10 s.\n\n"
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
