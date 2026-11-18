/*
 * ios3ds QEMU board: the Wi-Fi slot (ctr_wifi.c): TMIO controller 2 with an
 * AR6014-like SDIO card, its firmware's HTC/WMI protocol and a WPA2 access
 * point bridged to a QEMU NIC backend (-nic user,model=ctr-wifi).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once
#include "system/memory.h"

/* Create the device and map TMIO2 (0x10122000), the GPIO data registers
   (0x10147000) and CFG11_WIFICNT (0x10140180) into the shared IO container
   that starts at physical address io_shared_base. */
void ctr_wifi_init(MemoryRegion *io_shared, hwaddr io_shared_base);
