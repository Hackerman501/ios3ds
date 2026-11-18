/*
 * ios3ds ARM9: the 3DS system controller ("MCU", I2C bus 2, device 0x4A).
 *
 * The ARM9 owns the MCU: it reads it here and publishes everything through
 * the MCU mailbox in the shim region (boot/common/mcu_proto.h, docs/mcu.md):
 *   - RTC (regs 0x30-0x36, BCD), read once before XNU starts (the kernel's
 *     calendar is initialised from it) and every second afterwards;
 *   - battery percent / temperature / voltage and the charger flags
 *     (regs 0x0A-0x0F), every 2 s;
 *   - volume and 3D sliders (regs 0x08-0x09), every 100 ms;
 *   - IRQ events (regs 0x10-0x13, clear on read): HOME press/release, POWER
 *     press/held, shell close/open, every 30 ms. HOME and POWER become the
 *     INBTN_HOME / INBTN_POWER bits of the input samples (tsc.c);
 *   - accelerometer (enable reg 0x40, data regs 0x45-0x4A) while iOS wants it;
 *   - requests from the ARM11: power off (reg 0x20 bit 0) after iOS shut down.
 * Register map: libn3ds include/drivers/mcu_regmap.h / mcu.h, 3dbrew "I2C
 * Registers" (device 3), GodMode9 arm9 (RTC, battery, charging bit).
 *
 * Safe by default: if the MCU does not answer at boot, the mailbox says
 * MCU_ST_FAILED, one log line explains it and nothing else is touched; the
 * shim then behaves as before this driver (calendar at 1970, no battery).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mcu.h"
#include "i2c.h"
#include "log.h"
#include "util.h"
#include "mcu_proto.h"
#include "input_proto.h"

#define MCU_BUS     1           /* I2C2, 0x10144000 */
#define MCU_DEV     0x4A

#define R_VERSION   0x00
#define R_SLIDER3D  0x08        /* 0x08 3D slider, 0x09 volume slider */
#define R_BATT      0x0A        /* 0x0A temp, 0x0B percent, 0x0C fraction, 0x0D voltage, 0x0E ?, 0x0F flags */
#define R_IRQ       0x10        /* 4 bytes, clear on read */
#define R_IRQ_MASK  0x18        /* 4 bytes, 1 = masked */
#define R_PWR       0x20        /* bit 0 power off, bit 2 reboot */
#define R_LCD_PWR   0x22        /* bits 2/3 bottom backlight off/on, 4/5 top off/on (libn3ds MCU_LCD_PWR_*) */
#define R_LED       0x2D        /* notification LED pattern, 100-byte stream */
#define R_RTC       0x30        /* 7 bytes BCD */
#define R_ACC_CFG   0x40        /* bit 0 accelerometer on */
#define R_ACC_DATA  0x45        /* x, y, z: s16 little endian */

#define IRQ_POWER_PRESS   (1u << 0)
#define IRQ_POWER_HELD    (1u << 1)
#define IRQ_HOME_PRESS    (1u << 2)
#define IRQ_HOME_RELEASE  (1u << 3)
#define IRQ_SHELL_CLOSE   (1u << 5)
#define IRQ_SHELL_OPEN    (1u << 6)
#define IRQ_WANTED        (IRQ_POWER_PRESS | IRQ_POWER_HELD | IRQ_HOME_PRESS | IRQ_HOME_RELEASE | \
                           IRQ_SHELL_CLOSE | IRQ_SHELL_OPEN)

static volatile mcubox_t *box;
static u32 hz;                  /* ARM9 timer ticks per second */
static u32 fails_in_row;

/* timers (ARM9 ticks) */
static u32 t_irq, t_slider, t_batt, t_rtc, t_acc;

/* button state presented to the input mailbox */
static bool home_down, home_tap;
static u32 home_since;
static bool power_down;
static u32 power_until;         /* tick when the synthetic POWER press ends */
static bool power_long;
static u32 off_req, off_at;     /* power-off request, carried out at off_at */

static u32 ms_ticks(u32 ms) { return ms * hz / 1000u; }

static bool rd(u8 reg, u8 *out, u32 len)
{
    if (i2c_read(MCU_BUS, MCU_DEV, reg, out, len)) {
        fails_in_row = 0;
        return true;
    }
    box->i2c_errors++;
    if (++fails_in_row == 20) log_err("[mcu] 20 I2C transfers in a row failed (reg %02x): MCU readings frozen\n", reg);
    return false;
}

static bool wr(u8 reg, u8 v)
{
    if (i2c_write(MCU_BUS, MCU_DEV, reg, &v, 1)) return true;
    box->i2c_errors++;
    return false;
}

static void bump(void) { box->seq = box->seq + 1; }

static bool read_rtc(void)
{
    u8 raw[7];
    if (!rd(R_RTC, raw, 7)) return false;
    u32 t = mcu_rtc_to_unix(raw);
    for (int i = 0; i < 7; i++) box->rtc_raw[i] = raw[i];
    box->rtc_reads++;
    if (!t) return false;
    box->rtc_utc = t + (u32)box->rtc_offset;
    box->valid |= MCUV_RTC;
    return true;
}

static bool read_batt(void)
{
    u8 b[6];
    if (!rd(R_BATT, b, 6)) return false;
    box->batt_temp = b[0];
    box->batt_percent = b[1] > 100 ? 100 : b[1];
    box->batt_volt = b[3];
    box->flags = b[5];
    box->valid |= MCUV_BATT;
    return true;
}

static bool read_sliders(void)
{
    u8 b[2];
    if (!rd(R_SLIDER3D, b, 2)) return false;
    box->slider3d = b[0];
    box->vol_raw = b[1] & 0x3F;
    box->valid |= MCUV_SLIDERS;
    return true;
}

static const char *const wday[8] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "?" };

void mcu_init(u32 shim_pa, bool enable, int32_t rtc_offset, u32 ticks_per_s)
{
    if (!shim_pa) return;
    box = (volatile mcubox_t *)(uintptr_t)(shim_pa + SHIM_MCU_OFF);
    hz = ticks_per_s;
    for (u32 i = 0; i < sizeof(mcubox_t) / 4; i++) ((vu32 *)box)[i] = 0;
    box->version = MCU_VERSION;
    box->rtc_offset = rtc_offset;
    box->magic = MCU_MAGIC;
    if (!enable) {
        box->state = MCU_ST_DISABLED;
        log_warn("[mcu] disabled in boot.cfg (mcu=0): no RTC, battery or HOME/POWER from the MCU\n");
        return;
    }
    i2c_init(MCU_BUS);
    u8 v[2] = { 0xFF, 0xFF };
    if (!rd(R_VERSION, v, 2)) {
        box->state = MCU_ST_FAILED;
        log_err("[mcu] no answer on I2C bus 2 (device 4a, %u timeouts): clock stays at 1970, no battery, "
                "HOME/POWER unavailable\n", (unsigned)i2c_timeouts());
        return;
    }
    box->fw = (u32)v[0] << 8 | v[1];
    /* events we act on: unmask them, then drop whatever is pending from before */
    u32 mask = ~IRQ_WANTED;
    u8 m[4] = { (u8)mask, (u8)(mask >> 8), (u8)(mask >> 16), (u8)(mask >> 24) };
    bool mask_ok = i2c_write(MCU_BUS, MCU_DEV, R_IRQ_MASK, m, 4);
    u8 irq[4];
    rd(R_IRQ, irq, 4);
    bool rtc = read_rtc(), batt = read_batt(), sl = read_sliders();
    box->state = MCU_ST_OK;
    bump();
    const volatile u8 *r = box->rtc_raw;
    if (rtc)
        log_ok("[mcu] version %02x/%02x: RTC 20%02x-%02x-%02x %02x:%02x:%02x %s (offset %d s) = Unix %u\n",
               v[0], v[1], r[6], r[5], r[4], r[2], r[1], r[0], wday[r[3] & 7], (int)rtc_offset,
               (unsigned)box->rtc_utc);
    else
        log_warn("[mcu] version %02x/%02x: RTC unreadable (raw %02x %02x %02x %02x %02x %02x %02x): calendar stays at 1970\n",
                 v[0], v[1], r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
    logp("[mcu] battery %s%u%% %u mV %u C%s%s, volume slider %s%u/63, 3D slider %u, shell %s, IRQ mask %s\n",
         batt ? "" : "(unread) ", box->batt_percent, box->batt_volt * 20u, box->batt_temp,
         (box->flags & MCUF_ADAPTER) ? ", charger" : "", (box->flags & MCUF_CHARGING) ? ", charging" : "",
         sl ? "" : "(unread) ", box->vol_raw, box->slider3d,
         (box->flags & MCUF_SHELL_OPEN) ? "open" : "closed", mask_ok ? "set" : "NOT set (no HOME/POWER events)");
}

/* ---- notification LED (MCU reg 0x2D) -------------------------------------
   Pattern (3dbrew "MCU Services" / libn3ds McuInfoLedPattern): delay, smoothing,
   loop delay, unknown, then 32 steps of red, green, blue. Unverified on
   hardware, hence off by default (boot.cfg notify_led). */
static bool led_enabled;
static int led_shown = -1;              /* 0 off, 1 low-battery blink */

void mcu_set_led(bool on) { led_enabled = on; }

static void led_update(void)
{
    if (!led_enabled) return;
    int want = (box->valid & MCUV_BATT) && box->batt_percent <= 10 && !(box->flags & MCUF_CHARGING);
    if (want == led_shown) return;
    u8 p[100];
    for (u32 i = 0; i < sizeof p; i++) p[i] = 0;
    if (want) {
        p[0] = 0x2F;                        /* step time */
        p[1] = 0x2F;                        /* smoothing (fade between steps) */
        p[2] = 0x00;                        /* loop again at once */
        for (int i = 0; i < 32; i++) p[4 + i] = (i & 8) ? 0x00 : 0xA0;   /* red: on 8 steps, off 8 */
    }
    if (i2c_write(MCU_BUS, MCU_DEV, R_LED, p, sizeof p)) {
        led_shown = want;
        logp("[mcu] notification LED %s\n", want ? "blinking red: battery low" : "off");
    } else {
        box->i2c_errors++;
    }
}

void mcu_set_powersave(bool on) { if (box) box->ps_on = on; }
void mcu_screen_off(bool off) { if (box) box->screen_off = off; }

void mcu_config_acc(bool on)
{
    if (!box) return;
    box->acc_off = !on;
    if (!on) log_warn("[mcu] accelerometer hidden from iOS (boot.cfg mcu_acc=0)\n");
}

u16 mcu_buttons(void)
{
    u16 b = 0;
    if (home_down || home_tap) b |= INBTN_HOME;
    home_tap = false;               /* a tap is one sample down, then up */
    if (power_down) b |= INBTN_POWER;
    return b;
}

static void irq_events(u32 now)
{
    u8 b[4];
    if (!rd(R_IRQ, b, 4)) return;
    u32 irq = b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
    if (!irq) return;
    box->irq_seen |= irq;
    if (irq & IRQ_HOME_PRESS) {
        box->home_presses++;
        if (irq & IRQ_HOME_RELEASE) home_tap = true;    /* both in one poll */
        else { home_down = true; home_since = now; }
    } else if (irq & IRQ_HOME_RELEASE) {
        home_down = false;
    }
    /* POWER has no release event: a press is a tap (lock / wake); "held"
       (the MCU's long press) is held long enough for SpringBoard's 2.5 s
       timer to open the power-off slider: the input daemon sends the down
       after 0.8 s of its own loop time and SpringBoard's run loop lags, so
       6 s. Releasing late is harmless (lockButtonUp: ignores it once the
       power-down sheet is up). */
    if (irq & IRQ_POWER_PRESS) {
        box->power_presses++;
        if (!power_down) {
            power_down = true;
            power_long = false;
            power_until = now + ms_ticks(250);
        }
    }
    if (irq & IRQ_POWER_HELD) {
        box->power_holds++;
        power_down = true;
        power_long = true;
        power_until = now + ms_ticks(6000);
        logp("[mcu] POWER held: iOS power-off slider\n");
    }
    if (irq & (IRQ_SHELL_CLOSE | IRQ_SHELL_OPEN)) {
        if (irq & IRQ_SHELL_CLOSE) box->shell_closes++;
        if (irq & IRQ_SHELL_OPEN) box->shell_opens++;
        u8 f;
        if (rd(R_BATT + 5, &f, 1)) box->flags = f;
        /* closing locks iOS (a POWER tap) unless its screen is already off;
           opening wakes it (a HOME tap) if it is. backlight = 1 + level the
           shim last set from iOS' brightness (1 = off, 0 = not known yet). */
        u32 bl = box->backlight;
        bool open = box->flags & MCUF_SHELL_OPEN;
        const char *act = "";
        if (!open && bl != 1 && !power_down) {
            power_down = true;
            power_long = false;
            power_until = now + ms_ticks(250);
            act = ": locking iOS (POWER tap)";
        } else if (open && bl == 1) {
            home_tap = true;
            act = ": waking iOS (HOME tap)";
        }
        /* the backlights follow the shell whatever iOS does with the lock
           press (this build's SpringBoard keeps the screen on: SBDontLockEver) */
        bool bl_ok = wr(R_LCD_PWR, open ? 0x28 : 0x14);
        logp("[mcu] shell %s%s; LCD backlights %s%s\n", open ? "opened" : "closed", act,
             open ? "on" : "off", bl_ok ? "" : " (MCU write failed)");
    }
    bump();
}

static void power_off(u32 how)
{
    if (led_shown > 0) {                    /* don't leave the LED blinking on a switched-off console */
        led_enabled = true;
        box->batt_percent = 100;
        led_update();
    }
    log_warn("[mcu] iOS %s: log synced, powering off through the MCU\n",
             how == MCU_PWR_REBOOT ? "restarted" : "shut down");
    log_flush();
    if (!wr(R_PWR, 1)) log_err("[mcu] power-off write failed\n");
    log_flush();
}

void mcu_poll(u32 now)
{
    if (!box || box->state != MCU_ST_OK) return;
    box->polls = box->polls + 1;

    /* requests from the shim (line 4) */
    u32 rs = box->req_seq;
    if (rs != box->ack_seq) {
        u32 p = box->req_power;
        if (box->req_acc && !(box->valid & MCUV_ACC)) {
            if (wr(R_ACC_CFG, 1)) logp("[mcu] accelerometer on\n");
            box->valid |= MCUV_ACC;     /* "turned on", samples follow */
        }
        box->ack_seq = rs;
        static u32 delta_seq;
        if (box->user_delta_seq != delta_seq) {
            /* iOS set the time: keep RTC + offset across reboots (the 3DS RTC
               itself is not the user's time: Horizon adds an offset kept on
               the NAND, which we don't read; run14's RTC said 2001) */
            delta_seq = box->user_delta_seq;
            char t[16];
            int32_t total = box->rtc_offset + box->user_delta;
            int n = snprintf(t, sizeof(t), "%d\n", (int)total);
            int w = sd_write_small("0:/ios3ds/rtc_offset.txt", t, (u32)n);
            logp("[mcu] iOS set the time: rtc_offset %d saved to sd:/ios3ds/rtc_offset.txt (%s)\n",
                 (int)total, w == n ? "ok" : "write failed");
        }
        if ((p == MCU_PWR_OFF || p == MCU_PWR_REBOOT) && !off_req) {
            off_req = p;        /* let the monitor loop drain XNU's last lines first */
            off_at = now + ms_ticks(300);
        }
    }
    if (off_req && (int)(now - off_at) >= 0) {
        power_off(off_req);
        off_req = 0;
    }

    if (now - t_irq >= ms_ticks(30)) {
        t_irq = now;
        irq_events(now);
    }
    if (power_down && (int)(now - power_until) >= 0) {
        power_down = false;
        if (power_long) logp("[mcu] POWER hold released\n");
    }
    if (home_down && now - home_since >= 10 * hz) home_down = false;    /* lost release */

    if (now - t_slider >= ms_ticks(100)) {
        t_slider = now;
        u8 v = box->vol_raw, s = box->slider3d;
        if (read_sliders() && (v != box->vol_raw || s != box->slider3d)) bump();
    }
    if (now - t_batt >= 2 * hz) {
        t_batt = now;
        u8 p = box->batt_percent, f = box->flags;
        if (read_batt()) {
            if (p != box->batt_percent || ((f ^ box->flags) & (MCUF_ADAPTER | MCUF_CHARGING))) {
                logp("[mcu] battery %u%%%s%s\n", box->batt_percent,
                     (box->flags & MCUF_ADAPTER) ? ", charger" : "", (box->flags & MCUF_CHARGING) ? ", charging" : "");
            }
            bump();
        }
        led_update();
    }
    if (now - t_rtc >= hz) {
        t_rtc = now;
        if (read_rtc()) bump();
    }
    if (box->req_acc && now - t_acc >= ms_ticks(50)) {
        t_acc = now;
        u8 a[6];
        if (rd(R_ACC_DATA, a, 6)) {
            box->acc[0] = (int16_t)(a[0] | a[1] << 8);
            box->acc[1] = (int16_t)(a[2] | a[3] << 8);
            box->acc[2] = (int16_t)(a[4] | a[5] << 8);
            box->acc_samples++;
            bump();
        }
    }
}
