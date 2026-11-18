/*
 * Host test of the ARM9 MCU driver (boot/arm9/mcu.c) against a fake MCU
 * behind a fake I2C bus: RTC conversion, mailbox contents, HOME / POWER
 * button state, shell close/open policy, power-off request, dead-MCU
 * fallback. Built and run by tests/test_mcu.py.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "ios3ds.h"
#include "mcu_proto.h"
#include "input_proto.h"
#include "../../../boot/arm9/mcu.h"

/* ---- stand-ins for the monitor ------------------------------------------ */
void log_color(u32 color, const char *fmt, ...)
{
    va_list ap;
    (void)color;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    va_end(ap);
}
void log_flush(void) {}

/* ---- fake MCU ------------------------------------------------------------ */
static u8 regs[256];
static u32 irq;                 /* pending, clear on read */
static int dead, writes_0x20;

bool i2c_read(u8 bus, u8 dev, u8 reg, u8 *out, u32 len)
{
    if (dead || bus != 1 || dev != 0x4A) return false;
    for (u32 i = 0; i < len; i++, reg++) {
        if (reg >= 0x10 && reg < 0x14) {
            out[i] = irq >> ((reg - 0x10) * 8);
            irq &= ~(0xFFu << ((reg - 0x10) * 8));
        } else {
            out[i] = regs[reg];
        }
    }
    return true;
}

bool i2c_write(u8 bus, u8 dev, u8 reg, const u8 *in, u32 len)
{
    if (dead || bus != 1 || dev != 0x4A) return false;
    for (u32 i = 0; i < len; i++, reg++) {
        if (reg == 0x20) writes_0x20 = in[i];
        regs[reg] = in[i];
    }
    return true;
}

void i2c_init(u8 bus) { (void)bus; }
u32 i2c_timeouts(void) { return 0; }

/* ---- harness ------------------------------------------------------------- */
#define SHIM_PA 0x20840000u
#define HZ 1000u                /* 1 tick = 1 ms */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile mcubox_t *box(void) { return (volatile mcubox_t *)(SHIM_PA + SHIM_MCU_OFF); }

static u32 now;
static u16 run(u32 ms)          /* poll every 2 ms like the monitor; OR of all button samples */
{
    u16 seen = 0;
    for (u32 t = 0; t < ms; t += 2) {
        now += 2;
        mcu_poll(now);
        seen |= mcu_buttons();
    }
    return seen;
}

static void fake_reset(void)
{
    memset(regs, 0, sizeof regs);
    regs[0x00] = 0x13; regs[0x01] = 0x38;
    regs[0x0A] = 25; regs[0x0B] = 77; regs[0x0D] = 200; regs[0x0F] = 0x02 | 0x08 | 0x10;
    regs[0x08] = 0x80; regs[0x09] = 40;
    /* 2026-11-18 01:02:03, Wednesday */
    u8 rtc[7] = { 0x03, 0x02, 0x01, 0x03, 0x18, 0x11, 0x26 };
    memcpy(&regs[0x30], rtc, 7);
    irq = 0xFFFFFFFFu;          /* stale events from before the boot */
    dead = 0;
    writes_0x20 = -1;
}

static void test_rtc_math(void)
{
    static const struct { u8 raw[7]; u32 unix_time; } v[] = {
        { { 0x00, 0x00, 0x00, 0x04, 0x01, 0x01, 0x00 }, 946684800u },     /* 2000-01-01 */
        { { 0x59, 0x59, 0x23, 0x02, 0x29, 0x02, 0x00 }, 951868799u },     /* 2000-02-29 23:59:59 */
        { { 0x03, 0x02, 0x01, 0x03, 0x18, 0x11, 0x26 }, 1794963723u },    /* 2026-11-18 01:02:03 */
        { { 0x07, 0x06, 0x05, 0x01, 0x29, 0x02, 0x28 }, 1835413567u },    /* 2028-02-29 05:06:07 */
        { { 0x59, 0x59, 0x23, 0x04, 0x31, 0x12, 0x99 }, 4102444799u },    /* 2099-12-31 23:59:59 */
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
        CHECK(mcu_rtc_to_unix(v[i].raw) == v[i].unix_time, "rtc vector %u: %u != %u", i,
              mcu_rtc_to_unix(v[i].raw), v[i].unix_time);
    u8 bad[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(mcu_rtc_to_unix(bad) == 0, "0xFF RTC must be invalid");
    u8 bad2[7] = { 0x00, 0x60, 0x00, 0x00, 0x01, 0x01, 0x20 };
    CHECK(mcu_rtc_to_unix(bad2) == 0, "minute 60 must be invalid");
}

int main(void)
{
    void *m = mmap((void *)(uintptr_t)SHIM_PA, 0x10000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }

    test_rtc_math();

    /* boot: MCU answers */
    fake_reset();
    mcu_init(SHIM_PA, true, -3600, HZ);
    volatile mcubox_t *b = box();
    CHECK(b->magic == MCU_MAGIC && b->state == MCU_ST_OK, "state %u", b->state);
    CHECK(b->fw == 0x1338, "fw %x", b->fw);
    CHECK(b->rtc_utc == 1794963723u - 3600, "rtc_utc %u", b->rtc_utc);
    CHECK(b->batt_percent == 77 && (b->flags & MCUF_CHARGING) && (b->flags & MCUF_ADAPTER), "battery");
    CHECK(b->vol_raw == 40 && b->slider3d == 0x80, "sliders");
    CHECK((b->valid & (MCUV_RTC | MCUV_BATT | MCUV_SLIDERS)) == (MCUV_RTC | MCUV_BATT | MCUV_SLIDERS), "valid");
    u32 mask = regs[0x18] | regs[0x19] << 8 | regs[0x1A] << 16 | (u32)regs[0x1B] << 24;
    CHECK(mask == ~0x6Fu, "irq mask %08x", mask);
    CHECK(irq == 0, "stale IRQs not cleared at init");
    CHECK(run(100) == 0, "no buttons at rest");

    /* HOME: press ... release */
    irq |= 1u << 2;
    CHECK(run(100) & INBTN_HOME, "HOME down after press");
    CHECK(mcu_buttons() & INBTN_HOME, "HOME stays down");
    irq |= 1u << 3;
    run(100);
    CHECK(!(mcu_buttons() & INBTN_HOME), "HOME up after release");
    /* HOME tap inside one poll interval: one sample down, then up */
    irq |= (1u << 2) | (1u << 3);
    CHECK(run(100) & INBTN_HOME, "HOME tap seen");
    CHECK(!(mcu_buttons() & INBTN_HOME), "HOME tap is released");
    CHECK(b->home_presses == 2, "home presses %u", b->home_presses);

    /* POWER press: a ~250 ms tap */
    irq |= 1u << 0;
    run(60);
    CHECK(mcu_buttons() & INBTN_POWER, "POWER down after press");
    run(400);
    CHECK(!(mcu_buttons() & INBTN_POWER), "POWER tap released");
    /* POWER held: down for 4 s (SpringBoard's 2.5 s timer + the daemon's 0.8 s) */
    irq |= 1u << 1;
    run(5500);
    CHECK(mcu_buttons() & INBTN_POWER, "POWER still held at 5.5 s");
    run(1000);
    CHECK(!(mcu_buttons() & INBTN_POWER), "POWER hold released after 6 s");

    /* shell close with the screen on (backlight unknown) -> POWER tap */
    regs[0x0F] &= ~0x02;
    irq |= 1u << 5;
    CHECK(run(100) & INBTN_POWER, "shell close locks (POWER tap)");
    CHECK(regs[0x22] == 0x14, "shell close: backlights off (%02x)", regs[0x22]);
    run(400);
    /* shell open while the screen is off (shim wrote backlight = 1) -> HOME tap */
    b->backlight = 1;
    regs[0x0F] |= 0x02;
    irq |= 1u << 6;
    u16 seen = run(100);
    CHECK((seen & INBTN_HOME) && !(seen & INBTN_POWER), "shell open wakes (HOME tap): %x", seen);
    CHECK(regs[0x22] == 0x28, "shell open: backlights on (%02x)", regs[0x22]);
    /* shell close with the screen already off: nothing */
    regs[0x0F] &= ~0x02;
    irq |= 1u << 5;
    CHECK(run(500) == 0, "shell close with the screen off does nothing");

    /* sliders, battery and RTC follow the MCU */
    regs[0x09] = 10;
    regs[0x0B] = 15;
    regs[0x0F] &= ~0x18;
    regs[0x30] = 0x09;
    run(2100);
    CHECK(b->vol_raw == 10 && b->batt_percent == 15 && !(b->flags & MCUF_CHARGING), "updates");
    CHECK(b->rtc_utc == 1794963729u - 3600, "rtc update %u", b->rtc_utc);

    /* accelerometer: off until requested */
    regs[0x45] = 0x00; regs[0x46] = 0xC0;   /* x = -0x4000 */
    run(200);
    CHECK(b->acc_samples == 0 && regs[0x40] == 0, "accelerometer stays off");
    b->req_acc = 1;
    b->req_seq = b->req_seq + 1;
    run(200);
    CHECK(regs[0x40] == 1 && b->acc_samples > 0 && b->acc[0] == -0x4000, "accelerometer on, x %d", b->acc[0]);
    CHECK(b->ack_seq == b->req_seq, "request acknowledged");

    /* iOS shut down: power off through reg 0x20 */
    b->req_power = MCU_PWR_OFF;
    b->req_seq = b->req_seq + 1;
    run(100);
    CHECK(writes_0x20 == -1, "power off waits for the log to drain");
    run(300);
    CHECK(writes_0x20 == 1, "power off write %d", writes_0x20);

    /* MCU dead at boot: FAILED, no buttons, nothing touched */
    fake_reset();
    dead = 1;
    mcu_init(SHIM_PA, true, 0, HZ);
    CHECK(b->state == MCU_ST_FAILED && b->rtc_utc == 0, "dead MCU state %u", b->state);
    CHECK(run(100) == 0, "dead MCU: no buttons");

    /* boot.cfg mcu=0 */
    fake_reset();
    mcu_init(SHIM_PA, false, 0, HZ);
    CHECK(b->state == MCU_ST_DISABLED && regs[0x18] == 0, "disabled");

    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails != 0;
}
