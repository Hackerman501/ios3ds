/*
 * ios3ds ARM9: the 3DS I2C controllers, polled (the ARM9 has no I2C IRQ line).
 *
 * Register block (8 bytes per bus, libn3ds include/drivers/i2c.h, 3dbrew
 * "I2C Registers"): DATA u8 +0, CNT u8 +1, CNTEX u16 +2, SCL u16 +4.
 * CNT: bit0 stop, bit1 start, bit2 error/abort, bit4 ack, bit5 read,
 * bit6 IRQ enable, bit7 enable/busy.
 *
 * Transfer sequence = GodMode9's ARM9 I2C driver (arm9/source/system/i2c.c,
 * used on hardware for years) and libn3ds source/drivers/i2c.c:
 *   start + device address, ACK? -> register, ACK? -> [read: repeated start +
 *   device|1, ACK?] -> data bytes (read: ACK all but the last, last with STOP;
 *   write: last with STOP). A NACK aborts with CNT = EN|IRQ|ERROR|STOP and the
 *   whole transfer is retried, up to 8 times.
 * Bus setup = libn3ds I2C_init: CNTEX bit 1 (wait while the slave stretches
 * SCL), SCL = delays (5, 0). IRQ_EN stays set as in both drivers: the I2C
 * interrupts go to the ARM11 GIC (ids 84/85/92), which XNU never enables.
 *
 * Every wait is bounded: a dead bus costs at most a few ms per transfer and
 * is reported, never a hang of the monitor.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "i2c.h"

typedef struct {
    vu8  data;
    vu8  cnt;
    vu16 cntex;
    vu16 scl;
} i2c_regs_t;

#define I2C_STOP    (1u << 0)
#define I2C_START   (1u << 1)
#define I2C_ERROR   (1u << 2)
#define I2C_ACK     (1u << 4)
#define I2C_READ    (1u << 5)
#define I2C_IRQ     (1u << 6)
#define I2C_EN      (1u << 7)

#define I2C_SPIN    200000u     /* ~6 ms at 134 MHz: far above one byte at 100 kHz */

static u32 timeouts;

static i2c_regs_t *regs(u8 bus)
{
    static const u32 base[3] = { 0x10161000u, 0x10144000u, 0x10148000u };
    return (i2c_regs_t *)base[bus < 3 ? bus : 1];
}

static bool wait_busy(i2c_regs_t *r)
{
    for (u32 i = 0; r->cnt & I2C_EN; i++) {
        if (i > I2C_SPIN) {
            timeouts++;
            return false;
        }
    }
    return true;
}

/* send one byte; true = slave ACKed. On a NACK the transfer is aborted. */
static bool send(i2c_regs_t *r, u8 byte, u8 flags)
{
    r->data = byte;
    r->cnt = I2C_EN | I2C_IRQ | flags;
    if (!wait_busy(r)) return false;
    if (!(r->cnt & I2C_ACK)) {
        r->cnt = I2C_EN | I2C_IRQ | I2C_ERROR | I2C_STOP;
        wait_busy(r);
        return false;
    }
    return true;
}

/* device + sub-address bytes (+ repeated start for a read) */
static bool start_sub(i2c_regs_t *r, u8 dev, const u8 *sub, u32 sublen, bool read)
{
    for (int tries = 0; tries < 8; tries++) {
        u32 i;
        if (!wait_busy(r)) {
            r->cnt = I2C_EN | I2C_IRQ | I2C_ERROR | I2C_STOP;
            continue;
        }
        if (!send(r, dev, I2C_START)) continue;
        for (i = 0; i < sublen; i++)
            if (!send(r, sub[i], 0)) break;
        if (i < sublen) continue;
        if (read && !send(r, dev | 1u, I2C_START)) continue;
        return true;
    }
    return false;
}

static bool start(i2c_regs_t *r, u8 dev, u8 reg, bool read)
{
    return start_sub(r, dev, &reg, 1, read);
}

void i2c_init(u8 bus)
{
    i2c_regs_t *r = regs(bus);
    r->cntex = 2;                   /* libn3ds I2C_CLK_STRETCH */
    r->scl = 5u << 8;               /* libn3ds I2C_DELAYS(5, 0) */
}

bool i2c_read(u8 bus, u8 dev, u8 reg, u8 *out, u32 len)
{
    i2c_regs_t *r = regs(bus);
    if (!len || !start(r, dev, reg, true)) return false;
    while (--len) {
        r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_ACK;
        if (!wait_busy(r)) return false;
        *out++ = r->data;
    }
    r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_STOP;    /* last byte: NACK + stop */
    if (!wait_busy(r)) return false;
    *out = r->data;
    return true;
}

bool i2c_write(u8 bus, u8 dev, u8 reg, const u8 *in, u32 len)
{
    i2c_regs_t *r = regs(bus);
    if (!len || !start(r, dev, reg, false)) return false;
    while (--len)
        if (!send(r, *in++, 0)) return false;
    return send(r, *in, I2C_STOP);
}

/* 16-bit register index, big endian (Aptina MT9V113 cameras, docs/camera.md) */
bool i2c_read16(u8 bus, u8 dev, u16 idx, u8 *out, u32 len)
{
    i2c_regs_t *r = regs(bus);
    u8 sub[2] = { (u8)(idx >> 8), (u8)idx };
    if (!len || !start_sub(r, dev, sub, 2, true)) return false;
    while (--len) {
        r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_ACK;
        if (!wait_busy(r)) return false;
        *out++ = r->data;
    }
    r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_STOP;
    if (!wait_busy(r)) return false;
    *out = r->data;
    return true;
}

bool i2c_write16(u8 bus, u8 dev, u16 idx, const u8 *in, u32 len)
{
    i2c_regs_t *r = regs(bus);
    u8 sub[2] = { (u8)(idx >> 8), (u8)idx };
    if (!len || !start_sub(r, dev, sub, 2, false)) return false;
    while (--len)
        if (!send(r, *in++, 0)) return false;
    return send(r, *in, I2C_STOP);
}

/* no register index: start with the read address at once (GBATEK "3DS I2C
   New3DS C-Stick and ZL/ZR-Buttons": reading returns its report array) */
bool i2c_read_noreg(u8 bus, u8 dev, u8 *out, u32 len)
{
    i2c_regs_t *r = regs(bus);
    int tries;
    if (!len) return false;
    for (tries = 0; tries < 8; tries++) {
        if (!wait_busy(r)) {
            r->cnt = I2C_EN | I2C_IRQ | I2C_ERROR | I2C_STOP;
            continue;
        }
        if (send(r, dev | 1u, I2C_START)) break;
    }
    if (tries == 8) return false;
    while (--len) {
        r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_ACK;
        if (!wait_busy(r)) return false;
        *out++ = r->data;
    }
    r->cnt = I2C_EN | I2C_IRQ | I2C_READ | I2C_STOP;
    if (!wait_busy(r)) return false;
    *out = r->data;
    return true;
}

u32 i2c_timeouts(void) { return timeouts; }
