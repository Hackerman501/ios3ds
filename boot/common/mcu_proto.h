/*
 * ios3ds MCU mailbox: the 3DS system controller (MCU, I2C bus 2 device 0x4A),
 * read by the ARM9 monitor (boot/arm9/mcu.c), handed to iOS through the
 * kernel shim (tools/ios3ds-mcu/shim/mcu.c: RTC for the calendar, an
 * IOPMPowerSource for the battery, sysctl kern.ios3ds_mcu for the daemon) and
 * the userland daemon (tools/ios3ds-mcu/daemon: volume, brightness,
 * accelerometer, shell). HOME / POWER go through the input mailbox
 * (input_proto.h INBTN_HOME / INBTN_POWER). See docs/mcu.md.
 *
 * Only fixed-width C types: included by the ARM9, the ARM11 shim, the iOS
 * daemon and host tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/* Inside the shim region (SHIM_PA 0x20840000 / SHIM_VA 0x80840000):
   code < +0x5F00, probes +0x5F00, PC ring +0x5F80, blkmbox +0x6000,
   inmbox +0x6200..+0x63FF, **mcubox +0x6400..+0x64FF**, gmbox +0x7F00,
   bounce +0x8000. */
#define SHIM_MCU_OFF        0x6400u
#define MCU_MAGIC           0x3055434Du     /* 'MCU0' */
#define MCU_VERSION         1u
#define MCU_SYSCTL_NAME     "kern.ios3ds_mcu"

enum {                                      /* mcubox_t.state */
    MCU_ST_OFF = 0,         /* ARM9 has not run its MCU code (old monitor) */
    MCU_ST_OK = 1,          /* MCU answers; fields below are live (see `valid`) */
    MCU_ST_FAILED = 2,      /* no answer on I2C: everything falls back to the pre-MCU behaviour */
    MCU_ST_DISABLED = 3,    /* boot.cfg mcu=0 */
};

/* mcubox_t.valid: which readings the ARM9 got at least once */
#define MCUV_RTC        (1u << 0)
#define MCUV_BATT       (1u << 1)
#define MCUV_SLIDERS    (1u << 2)
#define MCUV_ACC        (1u << 3)

/* mcubox_t.flags (MCU register 0x0F, libn3ds / 3dbrew "I2C Registers") */
#define MCUF_SHELL_OPEN (1u << 1)
#define MCUF_ADAPTER    (1u << 3)
#define MCUF_CHARGING   (1u << 4)

/* mcubox_t.req_power */
enum { MCU_PWR_NONE = 0, MCU_PWR_OFF = 1, MCU_PWR_REBOOT = 2 };

typedef struct {
    /* line 0: ARM9 status */
    uint32_t magic;         /* MCU_MAGIC once the ARM9 runs this protocol */
    uint32_t version;       /* MCU_VERSION */
    uint32_t state;         /* MCU_ST_* */
    uint32_t fw;            /* MCU firmware version: reg 0x00 << 8 | reg 0x01 */
    uint32_t seq;           /* bumped after every update of lines 1-3 */
    uint32_t polls;         /* MCU poll rounds (liveness) */
    uint32_t i2c_errors;    /* failed transfers since boot */
    uint32_t valid;         /* MCUV_* */
    /* line 1: ARM9, real-time clock */
    uint32_t rtc_utc;       /* Unix time (UTC) = RTC + rtc_offset; 0 = unknown */
    int32_t  rtc_offset;    /* seconds added to the RTC (boot.cfg rtc_offset) */
    uint8_t  rtc_raw[8];    /* BCD: sec min hour wday mday mon year(2000+) + pad */
    uint32_t rtc_reads;
    uint32_t acc_off;       /* boot.cfg mcu_acc=0: the shim hides the accelerometer from iOS */
    uint32_t _r1[2];
    /* line 2: ARM9, battery, sliders, event counters */
    uint8_t  batt_percent;  /* reg 0x0B, 0..100 */
    uint8_t  batt_temp;     /* reg 0x0A, degrees C */
    uint8_t  batt_volt;     /* reg 0x0D, 20 mV units */
    uint8_t  flags;         /* MCUF_* (reg 0x0F) */
    uint8_t  vol_raw;       /* reg 0x09, volume slider 0..63 */
    uint8_t  slider3d;      /* reg 0x08, 3D slider 0..255 */
    uint8_t  _p2[2];
    uint32_t irq_seen;      /* OR of every MCU IRQ bit seen (regs 0x10-0x13) */
    uint32_t power_presses, power_holds, home_presses;
    uint32_t shell_closes, shell_opens;
    /* line 3: ARM9, accelerometer (regs 0x45-0x4A, raw s16, sampled while req_acc) */
    int16_t  acc[3];
    uint16_t _p3;
    uint32_t acc_samples;
    uint32_t _r3[5];
    /* line 4: ARM11 -> ARM9 requests (the shim cleans this line after writing) */
    uint32_t req_seq;       /* bumped after the fields below changed */
    uint32_t req_power;     /* MCU_PWR_*: iOS halted / restarted */
    uint32_t req_acc;       /* 1 = the accelerometer is wanted (enable + sample it) */
    uint32_t backlight;     /* 1 + LCD backlight level the shim last set (1 = screen off), 0 = never set */
    uint32_t _r4[4];
    /* line 5: ARM9 acknowledgements */
    uint32_t ack_seq;       /* == req_seq once the ARM9 acted on it */
    uint32_t ps_on;         /* boot.cfg powersave: the shim may mask VBlank while the CLCD is off */
    uint32_t screen_off;    /* 1 while the ARM9 sees iOS' screen off (power saving active) */
    uint32_t _r5[5];
    uint32_t _r6[16];       /* lines 6-7: spare */
} mcubox_t;

_Static_assert(sizeof(mcubox_t) == 256, "mcubox layout");
_Static_assert(__builtin_offsetof(mcubox_t, rtc_utc) == 32, "mcubox line 1");
_Static_assert(__builtin_offsetof(mcubox_t, batt_percent) == 64, "mcubox line 2");
_Static_assert(__builtin_offsetof(mcubox_t, acc) == 96, "mcubox line 3");
_Static_assert(__builtin_offsetof(mcubox_t, req_seq) == 128, "mcubox line 4");
_Static_assert(__builtin_offsetof(mcubox_t, ack_seq) == 160, "mcubox line 5");

/* ---- helpers shared by the ARM9 and the host tests ---------------------- */

static inline int mcu_bcd(uint8_t b)
{
    return (b >> 4) * 10 + (b & 15);
}

/* Days since 1970-01-01 for a proleptic Gregorian date (y >= 1970). */
static inline int32_t mcu_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* RTC registers 0x30-0x36 (BCD sec min hour wday mday mon year-2000) -> Unix
   seconds, or 0 if a field is out of range (MCU not answering = all 0xFF). */
static inline uint32_t mcu_rtc_to_unix(const uint8_t raw[7])
{
    for (int i = 0; i < 7; i++)
        if ((raw[i] & 15) > 9 || (raw[i] >> 4) > 9) return 0;
    int s = mcu_bcd(raw[0]), mi = mcu_bcd(raw[1]), h = mcu_bcd(raw[2]);
    int d = mcu_bcd(raw[4]), mo = mcu_bcd(raw[5]), y = 2000 + mcu_bcd(raw[6]);
    if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return 0;
    return (uint32_t)mcu_days_from_civil(y, mo, d) * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
}
