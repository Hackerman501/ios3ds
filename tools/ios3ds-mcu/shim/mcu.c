/*
 * ios3ds shim: the 3DS system controller (MCU) inside XNU. The ARM9 reads the
 * MCU (boot/arm9/mcu.c) into the MCU mailbox (boot/common/mcu_proto.h); this
 * file hands it to the kernel. See docs/mcu.md.
 *
 *   RTC     PEGetGMTTimeOfDay()      -> shim_mcu_get_gmt   (patch pe-get-gmt)
 *           IOKitInitializeTime's waitForService(IORTC) -> shim_mcu_wait_rtc
 *                                       (patch iortc-wait-mcu: skipped when
 *                                       the RTC is known, else the original
 *                                       2 s wait of iortc-wait-2s)
 *           PESetGMTTimeOfDay()      -> shim_mcu_set_gmt   (patch pe-set-gmt)
 *   halt    PEHaltRestart()'s gIOPlatform->haltRestart(type)
 *                                    -> shim_mcu_halt_restart (patch pe-halt-mcu):
 *                                       halt/restart = MCU power off by the ARM9
 *   battery an IOPMPowerSource ("ios3ds MCU battery"), created and refreshed
 *           from the sysctls below (thread context)
 *   sysctl  kern.ios3ds_mcu     RD, 256 bytes: the mailbox (mcubox_t)
 *           kern.ios3ds_mcuctl  RW, mcuctl_t: the daemon's requests
 *                                       (accelerometer on/off, LCD backlight)
 *
 * Kernel facts (iPod2,1 4.2.1 8C148; addresses from the kernelcache symbol
 * table, disassembly in docs/re-log.md section 30):
 *   PEGetGMTTimeOfDay 0x801c84d8, PESetGMTTimeOfDay 0x801c84f4,
 *   PEHaltRestart 0x801c98b0 (platform call at 0x801c98f6, vtable +0x388),
 *   IOKitInitializeTime 0x801a8004 (waitForService literal at 0x801a8030),
 *   gIOPlatform 0x8029d490 (vtable +0x390 getGMTTimeOfDay),
 *   IOPMPowerSource::powerSource 0x801bb624, IOService::attach 0x801adc54,
 *   IOService::registerService 0x801b0844, IOService::getPlatform 0x801ac744,
 *   IOService::getResourceService 0x801ac75c, ml_io_map 0x80067780.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ios3ds.h"
#include "mcu_proto.h"

/* ---- kernel KPIs (Thumb unless noted) ---------------------------------- */
#define KPT(addr, ret, ...) ((ret (*)(__VA_ARGS__))((addr) | 1))
#define IOLog                   KPT(0x801a6be8, void, const char *, ...)
#define ml_io_map               KPT(0x80067780, u32, u32, u32)
#define waitForService          KPT(0x801b18f0, void *, void *, void *)
#define getPlatform             KPT(0x801ac744, void *, void)
#define getResourceService      KPT(0x801ac75c, void *, void)
#define svc_attach              KPT(0x801adc54, int, void *, void *)
#define svc_registerService     KPT(0x801b0844, void, void *, u32)
#define ps_powerSource          KPT(0x801bb624, void *, void)
#define ps_setMaxCapacity       KPT(0x801bb3d0, void, void *, u32)
#define ps_setCurrentCapacity   KPT(0x801bb3fc, void, void *, u32)
#define ps_setIsCharging        KPT(0x801bb1f8, void, void *, int)
#define ps_setExternalConnected KPT(0x801bb288, void, void *, int)
#define ps_setExternalChargeCapable KPT(0x801bb258, void, void *, int)
#define ps_setBatteryInstalled  KPT(0x801bb228, void, void *, int)
#define ps_setVoltage           KPT(0x801bb348, void, void *, u32)
#define ps_setPSProperty        KPT(0x801bb0dc, void, void *, void *, void *)
#define ps_updateStatus         KPT(0x801bb428, void, void *)
#define sym_withCStringNoCopy   KPT(0x8019a5d0, void *, const char *)
#define num_withNumber          KPT(0x80197684, void *, u64, u32)
#define kOSBooleanTrue          (**(void ***)0x80232e4c)   /* a C++ reference: &gOSBooleanTrue */
#define kOSBooleanFalse         (**(void ***)0x80232e50)
#define gIOPlatform             (*(void **)0x8029d490)

struct sysctl_oid {
    void       *oid_parent;
    void       *oid_link;
    int         oid_number;
    int         oid_kind;
    void       *oid_arg1;
    int         oid_arg2;
    const char *oid_name;
    int       (*oid_handler)(struct sysctl_oid *, void *, int, void *req);
    const char *oid_fmt;
};
#define CTLFLAG_RD      0x80000000
#define CTLFLAG_WR      0x40000000
#define CTLTYPE_OPAQUE  5
#define OID_AUTO        (-1)
#define sysctl_register_oid   KPT(0x80159c54, void, struct sysctl_oid *)
#define sysctl_handle_opaque  KPT(0x80159242, int, struct sysctl_oid *, void *, int, void *)
#define SYSCTL_KERN_CHILDREN  ((void *)0x8027f7b8)

#define BOX ((volatile mcubox_t *)(SHIM_VA + SHIM_MCU_OFF))

static void dinval(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static void dclean(volatile void *p, u32 len)
{
    u32 a = (u32)p & ~31u, end = (u32)p + len;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* the ARM9 status lines (0-3, 5) of the mailbox, fresh from RAM */
static int mcu_live(void)
{
    volatile mcubox_t *b = BOX;
    dinval(b, 128);
    dinval(&b->ack_seq, 32);
    return b->magic == MCU_MAGIC && b->version == MCU_VERSION && b->state == MCU_ST_OK;
}

static int rtc_known(void)
{
    return mcu_live() && (BOX->valid & MCUV_RTC) && BOX->rtc_utc;
}

/* ---- RTC ------------------------------------------------------------------ */

static int32_t user_delta;          /* settimeofday() minus the RTC, until reboot */
static int gmt_logged;

/* PEGetGMTTimeOfDay(): seconds since 1970 (UTC). Without an MCU RTC: the
   original body (gIOPlatform->getGMTTimeOfDay(), 0 without a platform). */
long shim_mcu_get_gmt(void)
{
    if (rtc_known()) {
        long t = (long)(BOX->rtc_utc + (u32)user_delta);
        if (!gmt_logged++)
            IOLog("ios3ds-mcu: calendar from the 3DS RTC: %ld (20%02x-%02x-%02x %02x:%02x:%02x RTC, offset %d s)\n",
                  t, BOX->rtc_raw[6], BOX->rtc_raw[5], BOX->rtc_raw[4], BOX->rtc_raw[2], BOX->rtc_raw[1],
                  BOX->rtc_raw[0], (int)BOX->rtc_offset);
        return t;
    }
    if (!gmt_logged++)
        IOLog("ios3ds-mcu: no 3DS RTC (MCU state %u): calendar from the platform expert as before\n",
              (unsigned)BOX->state);
    void *pe = gIOPlatform;
    if (!pe) return 0;
    return ((long (*)(void *))(*(void ***)pe)[0x390 / 4])(pe);
}

/* PESetGMTTimeOfDay(secs): iOS set the clock (Settings, settimeofday). The
   3DS RTC is the console's own clock and is left alone; the difference is
   kept for later PEGetGMTTimeOfDay() calls (wake) until the next boot.
   AppleARMPE's version would block for 30 s waiting for an IORTC. */
void shim_mcu_set_gmt(long secs)
{
    if (rtc_known()) {
        user_delta = (int32_t)((u32)secs - BOX->rtc_utc);
        IOLog("ios3ds-mcu: iOS set the time to %ld (RTC %d s); the 3DS RTC is not written\n",
              secs, (int)user_delta);
    } else {
        IOLog("ios3ds-mcu: iOS set the time to %ld; no RTC to keep it\n", secs);
    }
}

/* IOKitInitializeTime(): waitForService(resourceMatching("IORTC"), {2 s}) runs
   right before clock_initialize_calendar(). Nothing publishes IORTC on the 3DS;
   with the MCU RTC the calendar does not depend on the platform expert any
   more, so the wait is skipped (the matching dictionary is released like
   waitForService would). Without it: the original call (iortc-wait-2s). */
void *shim_mcu_wait_rtc(void *matching, void *timeout)
{
    if (rtc_known()) {
        if (matching) ((void (*)(void *))(*(void ***)matching)[0x14 / 4])(matching);  /* ->release() */
        IOLog("ios3ds-mcu: 3DS RTC present: not waiting for IORTC\n");
        return 0;
    }
    return waitForService(matching, timeout);
}

/* ---- halt / restart ------------------------------------------------------- */

/* PEHaltRestart(type), after IOKit's shutdown handlers ran and the file
   systems were unmounted; r0 = gIOPlatform, r1 = type. kPEHaltCPU 0,
   kPERestartCPU 1, kPEUPSDelayHaltCPU 3: the ARM9 powers the console off
   through the MCU (a reboot would land in Luma/the HOME Menu, not iOS).
   Anything else (panic restart, hang, sync) goes to the platform as before. */
int shim_mcu_halt_restart(void *pe, unsigned type)
{
    volatile mcubox_t *b = BOX;
    if ((type == 0 || type == 1 || type == 3) && mcu_live()) {
        IOLog("ios3ds-mcu: %s: asking the ARM9 to power off through the MCU\n",
              type == 1 ? "restart" : "halt");
        b->req_power = type == 1 ? MCU_PWR_REBOOT : MCU_PWR_OFF;
        b->req_seq = b->req_seq + 1;
        dclean(&b->req_seq, 32);
        /* the ARM9 acknowledges, syncs its log and cuts power; this CPU waits.
           ~2-3 s of spinning without an answer: fall back to the platform. */
        for (u32 i = 0; i < 20000000u; i++) {
            dinval(&b->ack_seq, 32);
            if (b->ack_seq == b->req_seq) {
                for (;;) __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));   /* wait for power-off */
            }
        }
        IOLog("ios3ds-mcu: no answer from the ARM9; platform halt\n");
    }
    if (!pe) return -1;
    return ((int (*)(void *, unsigned))(*(void ***)pe)[0x388 / 4])(pe, type);
}

/* ---- battery: an IOPMPowerSource ------------------------------------------ */

static void *ps;
static u32 ps_seen = 0xFFFFFFFFu;
static void *sym_fully, *sym_rawv;

static void ps_update(void)
{
    volatile mcubox_t *b = BOX;
    if (!(b->valid & MCUV_BATT)) return;
    u32 key = b->batt_percent | (u32)b->flags << 8 | (u32)b->batt_volt << 16;
    if (ps && key == ps_seen) return;
    u32 pct = b->batt_percent, mv = b->batt_volt * 20u;
    int ext = (b->flags & MCUF_ADAPTER) != 0, chg = (b->flags & MCUF_CHARGING) != 0;
    if (!ps) {
        ps = ps_powerSource();
        if (!ps) return;
        sym_fully = sym_withCStringNoCopy("FullyCharged");
        sym_rawv = sym_withCStringNoCopy("AppleRawBatteryVoltage");
        void *prov = getPlatform();
        if (!prov) prov = getResourceService();
        svc_attach(ps, prov);
        ps_setBatteryInstalled(ps, 1);
        ps_setMaxCapacity(ps, 100);
    }
    ps_setCurrentCapacity(ps, pct);
    ps_setExternalConnected(ps, ext);
    ps_setExternalChargeCapable(ps, ext);
    ps_setIsCharging(ps, chg);
    ps_setVoltage(ps, mv);
    if (sym_fully) ps_setPSProperty(ps, sym_fully, (ext && !chg && pct >= 100) ? kOSBooleanTrue : kOSBooleanFalse);
    if (sym_rawv) {
        void *n = num_withNumber(mv, 32);
        if (n) {
            ps_setPSProperty(ps, sym_rawv, n);
            ((void (*)(void *))(*(void ***)n)[0x14 / 4])(n);           /* release (dict keeps it) */
        }
    }
    ps_updateStatus(ps);
    if (ps_seen == 0xFFFFFFFFu) {
        svc_registerService(ps, 0);
        IOLog("ios3ds-mcu: battery %u%%%s%s, %u mV: IOPMPowerSource registered\n", (unsigned)pct,
              ext ? ", charger" : "", chg ? ", charging" : "", (unsigned)mv);
    }
    ps_seen = key;
}

/* ---- LCD backlight ------------------------------------------------------- */
/* LCD registers (libn3ds include/arm11/drivers/lcd.h, 3dbrew "LCD Registers"):
   0x10202240 top-screen backlight PWM (brightness), 0x10202A40 bottom. Luma's
   screen init writes them from the ARM11; 0x10202000 is ARM11-only IO. */
#define LCD_PA          0x10202000u
#define LCD_ABL0        0x240u
#define LCD_ABL1        0xA40u
#define BL_MIN          0x10u
#define BL_MAX          0x8Fu       /* Luma's brightest preset is 0x5F; keep headroom but stay below 0x90 */

static vu32 *lcd;
static u32 bl_luma[2], bl_cur = 0xFFFFFFFFu;

static void backlight_set(u32 level)   /* level 0..1000 (iOS brightness x factor); 0 = off */
{
    if (!lcd) {
        u32 va = ml_io_map(LCD_PA, 0x1000);
        if (!va) return;
        lcd = (vu32 *)va;
        bl_luma[0] = lcd[LCD_ABL0 / 4];
        bl_luma[1] = lcd[LCD_ABL1 / 4];
        IOLog("ios3ds-mcu: LCD backlight as Luma left it: top 0x%x, bottom 0x%x\n",
              (unsigned)bl_luma[0], (unsigned)bl_luma[1]);
    }
    u32 v = level == 0 ? 0 : BL_MIN + (level > 1000 ? 1000 : level) * (BL_MAX - BL_MIN) / 1000;
    if (v == bl_cur) return;
    bl_cur = v;
    lcd[LCD_ABL0 / 4] = v;
    lcd[LCD_ABL1 / 4] = v;
    BOX->backlight = 1 + v;             /* the ARM9 uses it for shell close/open */
    dclean(&BOX->req_seq, 32);
}

/* ---- sysctls ------------------------------------------------------------- */

typedef struct {
    u32 acc;            /* 1 = accelerometer wanted */
    u32 backlight;      /* 1 + level 0..1000; 0 = leave the LCD alone */
    u32 _r[2];
} mcuctl_t;

static mcuctl_t ctl;
static u32 acc_sent;

static int mcu_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    (void)arg1; (void)arg2;
    if (mcu_live()) ps_update();
    return sysctl_handle_opaque(oidp, (void *)BOX, sizeof(mcubox_t), req);
}

static int mcuctl_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, void *req)
{
    int r = sysctl_handle_opaque(oidp, arg1, arg2, req);
    if (r) return r;
    volatile mcubox_t *b = BOX;
    if (!mcu_live()) return 0;
    if (ctl.acc != acc_sent) {
        acc_sent = ctl.acc;
        b->req_acc = ctl.acc;
        b->req_seq = b->req_seq + 1;
        dclean(&b->req_seq, 32);
    }
    if (ctl.backlight) backlight_set(ctl.backlight - 1);
    return 0;
}

static struct sysctl_oid mcu_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLTYPE_OPAQUE),
    .oid_arg1    = (void *)(SHIM_VA + SHIM_MCU_OFF),
    .oid_arg2    = (int)sizeof(mcubox_t),
    .oid_name    = "ios3ds_mcu",
    .oid_handler = mcu_sysctl,
    .oid_fmt     = "S,ios3ds_mcu",
};

static struct sysctl_oid mcuctl_oid = {
    .oid_parent  = SYSCTL_KERN_CHILDREN,
    .oid_number  = OID_AUTO,
    .oid_kind    = (int)(CTLFLAG_RD | CTLFLAG_WR | CTLTYPE_OPAQUE),
    .oid_arg1    = &ctl,
    .oid_arg2    = (int)sizeof(ctl),
    .oid_name    = "ios3ds_mcuctl",
    .oid_handler = mcuctl_sysctl,
    .oid_fmt     = "S,ios3ds_mcuctl",
};

/* thread context, idempotent (blk.c: first md0 request) */
void shim_mcu_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysctl_register_oid(&mcu_oid);
    sysctl_register_oid(&mcuctl_oid);
}
