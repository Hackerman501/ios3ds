"""Device-tree side of the ios3ds MCU drivers (imported by kernel/dtgen.py).
See docs/mcu.md section 6.2.

The n72 accelerometer node (/arm-io/i2c0/accelerometer, LIS302DL at I2C
address 0x1d, interrupt 0x63 on /arm-io/gpio) is kept, so Apple's
AppleEmbeddedAccelerometer (class AppleLIS302DL) matches and publishes the
IOHID accelerometer service SpringBoard and UIKit use. The kernel shim answers
its I2C transfers from the MCU's accelerometer samples
(tools/ios3ds-mcu/shim/mcu.c shim_mcu_iic). Its interrupt never fires: the
driver polls with its own timer at the ReportInterval SpringBoard sets.

SPDX-License-Identifier: GPL-3.0-or-later
"""

# nodes another module's DELETE list removes, kept for the MCU drivers
KEEP = ["arm-io/i2c0/accelerometer"]

ACCEL_I2C_ADDR = 0x1D       # must match ACC_I2C_ADDR in tools/ios3ds-mcu/shim/mcu.c


def apply(root):
    """Called by dtgen.generate() after the DELETE pass."""
    acc = root.find("arm-io/i2c0/accelerometer")
    if acc is None:
        return                  # i2c0 not kept (no audio module): no accelerometer
    compat = acc.get("compatible") or b""
    if b"accelerometer,lis302dl" not in compat:
        raise ValueError("dt_mcu: accelerometer is not a LIS302DL: %r" % compat)
    reg = acc.get("reg")
    if reg is None or int.from_bytes(reg[:4], "little") != ACCEL_I2C_ADDR:
        raise ValueError("dt_mcu: accelerometer I2C address is not %#x" % ACCEL_I2C_ADDR)
