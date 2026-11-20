/*
 * ios3ds ARM9: the 3DS gyroscope, for CoreMotion (docs/mcu.md section 11).
 *
 * The gyro sits on I2C bus 3 (0x10148000). libn3ds' device table
 * (source/arm11/drivers/i2c.c) lists three gyro addresses there, all
 * unverified on our hardware:
 *   0xD6  device  9  "unknown vendor"            -> tried as an ST L3G chip
 *   0xD0  device 10  InvenSense ITG-3270 (Old 3DS) -> ITG-3200 register map
 *   0xD2  device 11  "New 3DS only?"              -> tried as ITG and ST
 * Boot: probe them in that order (WHO_AM_I), put the one that answers to
 * sleep and record it in the MCU mailbox (gyro_state / gyro_dev / gyro_id).
 * While the shim sets req_gyro (iOS has an active gyro client: the
 * AppleLIS331DLH driver on the gyro node is powered, docs/mcu.md 11), the chip
 * is awake and read every 10 ms; samples go to the mailbox as deg/s (16.16)
 * in iOS device axes (boot.cfg gyro_axes). Otherwise it sleeps.
 *
 * ITG-3200 family (datasheet PS-ITG-3200A): WHO_AM_I 0x00, SMPLRT_DIV 0x15,
 * DLPF_FS 0x16 (FS_SEL must be 3: +-2000 deg/s, 14.375 LSB per deg/s - the
 * coefficient Horizon's HID reports for the Old 3DS), GYRO_XOUT_H 0x1D..0x22
 * (big endian), PWR_MGM 0x3E (bit 6 SLEEP, bits 2:0 clock).
 * ST L3G4200D / L3GD20: WHO_AM_I 0x0F (0xD3 / 0xD4 / 0xD7), CTRL_REG1 0x20
 * (0x0F = 100 Hz, on, xyz; 0x00 = power down), CTRL_REG4 0x23 (0x80 BDU |
 * 0x30 2000 deg/s, 70 mdeg/s per LSB), OUT_X_L 0x28 (| 0x80 auto-increment,
 * little endian).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gyro.h"
#include "i2c.h"
#include "log.h"
#include "mcu_proto.h"

#define GYRO_BUS    2           /* I2C3, 0x10148000 */

static volatile mcubox_t *box;
static u32 hz, t_sample, axes, fails;
static u8 dev, kind;            /* kind = GYRO_ST_ITG / GYRO_ST_ST once found */
static bool awake;

static bool wr(u8 reg, u8 v) { return i2c_write(GYRO_BUS, dev, reg, &v, 1); }

static bool probe(u8 d, u8 k)
{
    u8 id = 0;
    if (!i2c_read(GYRO_BUS, d, k == GYRO_ST_ITG ? 0x00 : 0x0F, &id, 1)) return false;
    if (k == GYRO_ST_ST && (id & 0xF0) != 0xD0) return false;   /* 0xD3 / 0xD4 / 0xD7 */
    dev = d;
    kind = k;
    box->gyro_dev = d;
    box->gyro_id = id;
    return true;
}

static bool set_awake(bool on)
{
    bool ok;
    if (kind == GYRO_ST_ITG)
        ok = on ? wr(0x3E, 0x01) && wr(0x15, 9) && wr(0x16, 0x1B)     /* PLL x gyro; 1 kHz / 10; FS 3, 42 Hz LPF */
                : wr(0x3E, 0x40);                                      /* SLEEP */
    else
        ok = on ? wr(0x23, 0xB0) && wr(0x20, 0x0F)                     /* BDU, 2000 deg/s; 100 Hz, on, xyz */
                : wr(0x20, 0x00);                                      /* power down */
    awake = on;
    return ok;
}

void gyro_init(u32 shim_pa, bool enable, const char *axes_cfg, u32 ticks_per_s)
{
    if (!shim_pa) return;
    box = (volatile mcubox_t *)(uintptr_t)(shim_pa + SHIM_MCU_OFF);
    hz = ticks_per_s;
    axes = mcu_axes_parse(GYRO_AXES_DEFAULT);
    if (axes_cfg && *axes_cfg) {
        u32 m = mcu_axes_parse(axes_cfg);
        if (m) axes = m;
        else log_warn("[gyro] boot.cfg gyro_axes=%s not understood (want e.g. -y,x,z): default %s\n",
                      axes_cfg, GYRO_AXES_DEFAULT);
    }
    if (!enable) {
        box->gyro_state = GYRO_ST_DISABLED;
        log_warn("[gyro] disabled in boot.cfg (gyro=0): no gyroscope for iOS\n");
        return;
    }
    i2c_init(GYRO_BUS);
    if (probe(0xD6, GYRO_ST_ST) || probe(0xD0, GYRO_ST_ITG) || probe(0xD2, GYRO_ST_ITG) || probe(0xD2, GYRO_ST_ST)) {
        bool ok = set_awake(false);
        box->gyro_state = kind;
        log_ok("[gyro] %s at I2C 3:%02x (WHO_AM_I %02x)%s, axes %s\n",
               kind == GYRO_ST_ITG ? "ITG-3200 family" : "ST L3G family", dev, box->gyro_id,
               ok ? ", asleep" : ", sleep write FAILED", axes_cfg && *axes_cfg ? axes_cfg : GYRO_AXES_DEFAULT);
    } else {
        box->gyro_state = GYRO_ST_ABSENT;
        log_warn("[gyro] no gyroscope on I2C bus 3 (d6/d0/d2): CoreMotion gets no rotation rates\n");
    }
}

static void sample(void)
{
    u8 r[6];
    if (!i2c_read(GYRO_BUS, dev, kind == GYRO_ST_ITG ? 0x1D : 0xA8, r, 6)) {
        if (++fails == 50) log_err("[gyro] 50 reads in a row failed: rotation rates frozen\n");
        return;
    }
    fails = 0;
    int32_t raw[3], out[3];
    for (int i = 0; i < 3; i++)
        raw[i] = kind == GYRO_ST_ITG ? (int16_t)(r[2 * i] << 8 | r[2 * i + 1])
                                     : (int16_t)(r[2 * i] | r[2 * i + 1] << 8);
    u32 q = kind == GYRO_ST_ITG ? GYRO_Q10_ITG : GYRO_Q10_ST;
    for (int i = 0; i < 3; i++) raw[i] = mcu_gyro_fixed(raw[i], q);
    mcu_axes_apply(axes, raw, out);
    for (int i = 0; i < 3; i++) box->gyro[i] = out[i];
    box->gyro_samples = box->gyro_samples + 1;
}

void gyro_poll(u32 now)
{
    if (!box || (box->gyro_state != GYRO_ST_ITG && box->gyro_state != GYRO_ST_ST)) return;
    bool want = box->req_gyro != 0;
    if (want != awake) {
        bool ok = set_awake(want);
        static u32 changes;
        if (++changes <= 20 || !ok)
            logp("[gyro] %s by iOS%s (%u samples so far)\n", want ? "on" : "off",
                 ok ? "" : ": I2C write FAILED", (unsigned)box->gyro_samples);
        t_sample = now;
    }
    if (awake && now - t_sample >= hz / 100u) {
        t_sample = now;
        sample();
    }
}
