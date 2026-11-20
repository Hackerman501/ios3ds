/*
 * ios3ds ARM9: the New 3DS "extra HID" chip: C-stick and ZL/ZR buttons
 * (docs/n3ds.md section 3).
 *
 * I2C bus 3 (0x10148000), device address 0x54 (libn3ds I2C_DEV_EXTHID = 17,
 * {I2C_BUS3, 0x2A << 1}). It has no register index: a read returns its
 * report (GBATEK "3DS I2C New3DS C-Stick and ZL/ZR-Buttons", 3dbrew
 * "I2C Registers" device 17):
 *   [0] status 0x80..0x83 (0x80 after power-up)
 *   [1] buttons: ZL = bit 2, ZR = bit 1 (3dbrew; GBATEK only knows ZL)
 *   [2] [3] C-stick X / Y, signed, about +-0x1x (right / up positive)
 *   [4] 0xFF
 *   [5] [6] C-stick X / Y, signed, about +-0x7x (finer)
 * Reading more than 0x1A bytes can hang the chip (GBATEK), so we read 7.
 * The bus needs CNTEX "wait while SCL is held low" (i2c_init does it).
 * Horizon reads it through ir:rst; the chip also pulls GPIO3 bit 0 / ARM11
 * IRQ 0x68 on changes, which we do not use: the ARM9 polls at ~60 Hz.
 * Nothing about the axes' signs and range is confirmed on our hardware yet:
 * with boot.cfg diag=1 every changed report is logged raw.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "exthid.h"
#include "i2c.h"
#include "log.h"
#include "input_proto.h"

#define EXT_BUS     2           /* I2C3 */
#define EXT_DEV     0x54

static bool on, diag;
static u32 hz, t_last, fails, state, logged;
static u8 last_raw[7];

static bool read_report(u8 r[7]) { return i2c_read_noreg(EXT_BUS, EXT_DEV, r, 7); }

static u32 parse(const u8 r[7])
{
    int x, y;
    if (r[4] == 0xFF) { x = (int8_t)r[5]; y = (int8_t)r[6]; }
    else { x = (int8_t)r[2] * 4; y = (int8_t)r[3] * 4; }       /* coarse bytes only */
    if (x > 127) x = 127;
    if (x < -127) x = -127;
    if (y > 127) y = 127;
    if (y < -127) y = -127;
    return INEXT_VALID | ((u32)r[0] << 16) | ((u32)(u8)y << 8) | (u8)x |
           ((r[1] & 4u) ? INEXT_ZL : 0) | ((r[1] & 2u) ? INEXT_ZR : 0);
}

void exthid_init(bool enable, bool diag_, u32 ticks_per_s)
{
    u8 r[7];
    hz = ticks_per_s;
    diag = diag_;
    if (!enable) return;
    i2c_init(EXT_BUS);
    if (!read_report(r) || (r[0] & 0xFC) != 0x80) {
        log_warn("[exthid] no C-stick/ZL/ZR chip at I2C 3:%02x (%s, status %02x): New 3DS extras off\n",
                 EXT_DEV, i2c_timeouts() ? "bus timeout" : "no ACK / bad status", r[0]);
        return;
    }
    on = true;
    state = parse(r);
    for (int i = 0; i < 7; i++) last_raw[i] = r[i];
    log_ok("[exthid] C-stick + ZL/ZR at I2C 3:%02x: %02x %02x %02x %02x %02x %02x %02x\n",
           EXT_DEV, r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
}

void exthid_poll(u32 now)
{
    u8 r[7];
    if (!on || now - t_last < hz / 60u) return;
    t_last = now;
    if (!read_report(r) || (r[0] & 0xFC) != 0x80) {
        if (++fails == 60) log_err("[exthid] 60 reads in a row failed: C-stick / ZL / ZR released\n");
        if (fails >= 60) state = 0;
        return;
    }
    fails = 0;
    u32 s = parse(r);
    bool changed = false;
    for (int i = 0; i < 7; i++) changed |= r[i] != last_raw[i];
    if (changed && ((s ^ state) & (INEXT_ZL | INEXT_ZR) ? logged < 20 : diag && logged < 400)) {
        logged++;
        logp("[exthid] %02x %02x %02x %02x %02x %02x %02x -> x %d y %d%s%s\n", r[0], r[1], r[2], r[3], r[4], r[5],
             r[6], (int)(int8_t)(s & 0xFF), (int)(int8_t)((s >> 8) & 0xFF), (s & INEXT_ZL) ? " ZL" : "",
             (s & INEXT_ZR) ? " ZR" : "");
    }
    for (int i = 0; i < 7; i++) last_raw[i] = r[i];
    state = s;
}

u32 exthid_state(void) { return on ? state : 0; }
