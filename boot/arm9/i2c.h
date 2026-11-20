/* ios3ds ARM9: 3DS I2C controllers (polled). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ios3ds.h"

/* bus 0..2 = I2C1 0x10161000, I2C2 0x10144000, I2C3 0x10148000;
   dev = 8-bit device address (write form, e.g. 0x4A for the MCU). */
bool i2c_read(u8 bus, u8 dev, u8 reg, u8 *out, u32 len);
bool i2c_write(u8 bus, u8 dev, u8 reg, const u8 *in, u32 len);
void i2c_init(u8 bus);
/* 16-bit big-endian register index (Aptina cameras) */
bool i2c_read16(u8 bus, u8 dev, u16 idx, u8 *out, u32 len);
bool i2c_write16(u8 bus, u8 dev, u16 idx, const u8 *in, u32 len);
u32 i2c_timeouts(void);         /* busy waits that ran out (bus never answered) */
