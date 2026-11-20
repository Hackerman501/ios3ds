/*
 * ios3ds QEMU board: 3DS cameras (ctr_cam.c, docs/camera.md section 5).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "system/memory.h"

/* CAM blocks at shared + regs_off (+0x1000 for CAM1); FIFO windows on sys */
void ctr_cam_init(MemoryRegion *shared, hwaddr regs_off, MemoryRegion *sys);
/* CFG11_CAMERA_CNT bit 0: the sensors answer on I2C only while it is set */
void ctr_cam_clock(bool on);
/* I2C controller hooks (bus 0..2 as in nintendo3ds.c, 8-bit address) */
bool ctr_cam_i2c_present(int bus, uint8_t addr);
bool ctr_cam_i2c_start(int bus, uint8_t addr);
bool ctr_cam_i2c_write(int bus, uint8_t byte);
uint8_t ctr_cam_i2c_read(int bus);
uint64_t ctr_cam_frames(void);
