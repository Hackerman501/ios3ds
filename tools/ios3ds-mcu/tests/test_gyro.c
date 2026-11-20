/*
 * Host test of the ARM9 gyroscope driver (boot/arm9/gyro.c) and the shared
 * conversion helpers (boot/common/mcu_proto.h): raw counts -> deg/s 16.16,
 * the boot.cfg gyro_axes map, chip probing on a fake I2C bus 3 (ITG-3200 at
 * 0xD0, or an ST L3G at 0xD6), wake / sleep as iOS asks, 100 Hz sampling.
 * Built and run by tests/test_mcu.py.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "ios3ds.h"
#include "mcu_proto.h"
#include "../../../boot/arm9/gyro.h"

void log_color(u32 color, const char *fmt, ...)
{
    va_list ap;
    (void)color;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- fake bus 3 ----------------------------------------------------------- */
static u8 chip_dev;             /* 0xD0 = ITG, 0xD6 = ST, 0 = none */
static u8 regs[0x40];
static int16_t raw[3];          /* chip-axis counts */
static u32 data_reads;

bool i2c_read(u8 bus, u8 dev, u8 reg, u8 *out, u32 len)
{
    if (bus != 2 || dev != chip_dev) return false;
    if (chip_dev == 0xD6) reg &= 0x7F;      /* auto-increment bit */
    for (u32 i = 0; i < len; i++, reg++) {
        u8 v = regs[reg & 0x3F];
        if (chip_dev == 0xD0 && reg >= 0x1D && reg <= 0x22)
            v = (reg - 0x1D) & 1 ? (u8)raw[(reg - 0x1D) / 2] : (u8)((u16)raw[(reg - 0x1D) / 2] >> 8);
        if (chip_dev == 0xD6 && reg >= 0x28 && reg <= 0x2D)
            v = (reg - 0x28) & 1 ? (u8)((u16)raw[(reg - 0x28) / 2] >> 8) : (u8)raw[(reg - 0x28) / 2];
        if (reg == 0x1D || reg == 0x28) data_reads++;
        out[i] = v;
    }
    return true;
}

bool i2c_write(u8 bus, u8 dev, u8 reg, const u8 *in, u32 len)
{
    if (bus != 2 || dev != chip_dev) return false;
    for (u32 i = 0; i < len; i++) regs[(reg + i) & 0x3F] = in[i];
    return true;
}

void i2c_init(u8 bus) { (void)bus; }

/* ---- harness ------------------------------------------------------------- */
#define SHIM_PA 0x20840000u
#define HZ 1000u
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf(__VA_ARGS__); printf("\n"); } } while (0)
static volatile mcubox_t *box(void) { return (volatile mcubox_t *)(SHIM_PA + SHIM_MCU_OFF); }
static u32 now;
static void run(u32 ms) { for (u32 t = 0; t < ms; t += 2) { now += 2; gyro_poll(now); } }

static void reset(u8 dev)
{
    memset((void *)box(), 0, sizeof(mcubox_t));
    memset(regs, 0, sizeof regs);
    chip_dev = dev;
    regs[0x00] = 0x68;          /* ITG WHO_AM_I */
    regs[0x0F] = 0xD4;          /* L3GD20 WHO_AM_I */
    data_reads = 0;
}

static double dps(int32_t fixed) { return fixed / 65536.0; }

static void test_math(void)
{
    /* 14.375 counts = 1 deg/s on the ITG */
    CHECK(fabs(dps(mcu_gyro_fixed(14375, GYRO_Q10_ITG)) - 1000) < 0.01, "ITG 1000 deg/s: %f",
          dps(mcu_gyro_fixed(14375, GYRO_Q10_ITG)));
    CHECK(fabs(dps(mcu_gyro_fixed(-32768, GYRO_Q10_ITG)) + 2279.51) < 0.05, "ITG full scale: %f",
          dps(mcu_gyro_fixed(-32768, GYRO_Q10_ITG)));
    CHECK(fabs(dps(mcu_gyro_fixed(1000, GYRO_Q10_ST)) - 70) < 0.001, "ST 70 deg/s");
    CHECK(fabs(dps(mcu_gyro_fixed(-1, GYRO_Q10_ITG)) + 1 / 14.375) < 1e-4, "ITG one count");
    CHECK(mcu_gyro_fixed(0, GYRO_Q10_ITG) == 0, "zero");
    CHECK(mcu_gyro_fixed(-5, GYRO_Q10_ITG) == -mcu_gyro_fixed(5, GYRO_Q10_ITG), "symmetric rounding");

    uint32_t m = mcu_axes_parse("-y,x,z");
    int32_t in[3] = { 1, 2, 3 }, out[3];
    CHECK(m != 0, "parse default");
    mcu_axes_apply(m, in, out);
    CHECK(out[0] == -2 && out[1] == 1 && out[2] == 3, "map -y,x,z: %d %d %d", out[0], out[1], out[2]);
    m = mcu_axes_parse(" z , -x,+y");
    mcu_axes_apply(m, in, out);
    CHECK(m && out[0] == 3 && out[1] == -1 && out[2] == 2, "map z,-x,+y: %d %d %d", out[0], out[1], out[2]);
    CHECK(!mcu_axes_parse("x,x,z"), "duplicate axis must fail");
    CHECK(!mcu_axes_parse("x,y"), "two axes must fail");
    CHECK(!mcu_axes_parse("x,y,z,"), "trailing junk must fail");
    CHECK(!mcu_axes_parse("a,b,c"), "bad letters must fail");
}

int main(void)
{
    void *mm = mmap((void *)(uintptr_t)SHIM_PA, 0x10000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mm == MAP_FAILED) { perror("mmap"); return 2; }
    volatile mcubox_t *b = box();
    test_math();

    /* ITG-3270 at 0xD0: found, put to sleep, no sampling until iOS asks */
    reset(0xD0);
    gyro_init(SHIM_PA, true, NULL, HZ);
    CHECK(b->gyro_state == GYRO_ST_ITG && b->gyro_dev == 0xD0 && b->gyro_id == 0x68, "ITG probe: state %u dev %x",
          b->gyro_state, b->gyro_dev);
    CHECK(regs[0x3E] & 0x40, "ITG asleep after boot");
    run(100);
    CHECK(data_reads == 0 && b->gyro_samples == 0, "no reads while not wanted");

    /* iOS wants it: awake, FS_SEL 3, ~100 Hz */
    raw[0] = 1438; raw[1] = -2875; raw[2] = 0;      /* 100 and -200 deg/s about chip x, y */
    b->req_gyro = 1;
    run(1000);
    CHECK(!(regs[0x3E] & 0x40) && (regs[0x16] & 0x18) == 0x18, "ITG awake, FS 3: PWR %02x DLPF %02x", regs[0x3E], regs[0x16]);
    CHECK(b->gyro_samples >= 95 && b->gyro_samples <= 101, "100 Hz: %u samples in 1 s", b->gyro_samples);
    /* default map -y,x,z: iOS x = -chip y, iOS y = chip x */
    CHECK(fabs(dps(b->gyro[0]) - 200) < 0.01, "iOS x %f", dps(b->gyro[0]));
    CHECK(fabs(dps(b->gyro[1]) - 100) < 0.05, "iOS y %f", dps(b->gyro[1]));
    CHECK(b->gyro[2] == 0, "iOS z %d", b->gyro[2]);

    /* iOS done: back to sleep, sampling stops */
    b->req_gyro = 0;
    run(10);
    u32 n = b->gyro_samples;
    run(500);
    CHECK(regs[0x3E] & 0x40, "ITG asleep again");
    CHECK(b->gyro_samples == n, "no samples while asleep");

    /* ST chip at 0xD6, custom axes */
    reset(0xD6);
    gyro_init(SHIM_PA, true, "x,y,-z", HZ);
    CHECK(b->gyro_state == GYRO_ST_ST && b->gyro_dev == 0xD6 && b->gyro_id == 0xD4, "ST probe: state %u", b->gyro_state);
    CHECK(regs[0x20] == 0, "ST powered down after boot");
    raw[0] = 0; raw[1] = 0; raw[2] = 1000;          /* 70 deg/s about z */
    b->req_gyro = 1;
    run(100);
    CHECK(regs[0x20] == 0x0F && (regs[0x23] & 0x30) == 0x30, "ST on, 2000 dps: %02x %02x", regs[0x20], regs[0x23]);
    CHECK(fabs(dps(b->gyro[2]) + 70) < 0.01, "ST z negated: %f", dps(b->gyro[2]));

    /* bad axes string: default kept */
    reset(0xD0);
    gyro_init(SHIM_PA, true, "q,r,s", HZ);
    raw[0] = 1438; raw[1] = 0; raw[2] = 0;
    b->req_gyro = 1;
    run(50);
    CHECK(fabs(dps(b->gyro[1]) - 100) < 0.05, "default map after a bad gyro_axes");

    /* nothing on the bus; boot.cfg gyro=0 */
    reset(0);
    gyro_init(SHIM_PA, true, NULL, HZ);
    CHECK(b->gyro_state == GYRO_ST_ABSENT, "absent: %u", b->gyro_state);
    b->req_gyro = 1;
    run(100);
    CHECK(b->gyro_samples == 0, "absent: no samples");
    reset(0xD0);
    gyro_init(SHIM_PA, false, NULL, HZ);
    CHECK(b->gyro_state == GYRO_ST_DISABLED && regs[0x3E] == 0, "gyro=0: chip untouched");

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
