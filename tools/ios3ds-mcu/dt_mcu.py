"""Device-tree side of the ios3ds MCU drivers (imported by kernel/dtgen.py).
See docs/mcu.md section 6.2.

The n72 accelerometer node (/arm-io/i2c0/accelerometer, LIS302DL at I2C
address 0x1d, interrupt 0x63 on /arm-io/gpio) is kept, so Apple's
AppleEmbeddedAccelerometer (class AppleLIS302DL) matches and publishes the
IOHID accelerometer service SpringBoard and UIKit use. The kernel shim answers
its I2C transfers from the MCU's accelerometer samples
(tools/ios3ds-mcu/shim/mcu.c shim_mcu_iic). Its interrupt never fires: the
driver polls with its own timer at the ReportInterval SpringBoard sets.

A gyro node is added next to it (/arm-io/i2c0/gyro, docs/mcu.md section 11):
compatible 'accelerometer,lis331dlh', so the same kext starts its
AppleLIS331DLH class on it, a class the n72 does not otherwise use. Two words
of that class' vtable are patched (kernel/patches-mcu.json gyro-usage /
gyro-dispatch): the service registers as HID usage 0xff00/9 and dispatches
IOHIDEvent type 20 (gyro) with the ARM9's samples, which is what CoreMotion
looks for. The node is named 'gyro' as on the iPhone 4: CoreMotion's
CLSensorInterfaceIoHid::getGyroDt looks up the service named 'gyro' for
'gyro-interrupt-calibration' (8 bytes: u16 version 1, u16, u16 sample period
in us, u16) and scales the requested interval by period / 5 ms. Without it,
deviceMotion logs "Could not get gyro dt" and assumes 4.76 ms. Our samples
come exactly at the ReportInterval, so the period is 5000 us (dt = interval).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import struct

# nodes another module's DELETE list removes, kept for the MCU drivers
KEEP = ["arm-io/i2c0/accelerometer"]

ACCEL_I2C_ADDR = 0x1D       # must match ACC_I2C_ADDR in tools/ios3ds-mcu/shim/mcu.c
GYRO_I2C_ADDR = 0x6B        # must match GYRO_I2C_ADDR in tools/ios3ds-mcu/shim/mcu.c (free on i2c0)
GYRO_GPIO = 0x62            # its (never firing) interrupt on /arm-io/gpio: unused by the n72 nodes


def add_gyro(acc, i2c0):
    """A sibling of the accelerometer node, matched by AppleLIS331DLH."""
    from fwlib.appledt import Node
    phandles = set()

    def walk(n):
        ph = n.get("AAPL,phandle")
        if ph:
            phandles.add(int.from_bytes(ph[:4], "little"))
        for c in n.children:
            walk(c)
    walk(i2c0)
    ph = int.from_bytes(acc.get("AAPL,phandle")[:4], "little") + 0x100
    while ph in phandles:
        ph += 4
    g = Node()
    g.set("name", b"gyro\0")
    g.set("compatible", b"accelerometer,lis331dlh\0")
    g.set("device_type", b"gyro\0")
    g.set("reg", struct.pack("<4I", GYRO_I2C_ADDR, 0x9c4, 0, 0))
    g.set("interrupt-parent", acc.get("interrupt-parent"))
    g.set("interrupts", struct.pack("<2I", GYRO_GPIO, 1))
    g.set("gyro-interrupt-calibration", struct.pack("<4H", 1, 0, 5000, 0))
    g.set("AAPL,phandle", struct.pack("<I", ph))
    i2c0.children.append(g)


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
    i2c0 = root.find("arm-io/i2c0")
    for c in i2c0.children:
        r = c.get("reg")
        if c.name == "gyro" or (r and int.from_bytes(r[:4], "little") == GYRO_I2C_ADDR):
            raise ValueError("dt_mcu: i2c0 already has a gyro / address %#x" % GYRO_I2C_ADDR)
    add_gyro(acc, i2c0)
