#!/usr/bin/env python3
"""Self-test for the IO models of the `nintendo3ds` QEMU machine.

  emu/qemu/tests/iotest.py            (needs build/qemu and an arm-none-eabi-gcc)

Part 1 runs arm9test.c on the emulated ARM9 (CFG9, TMIO1 gating/status, ARM9
IRQ controller and timers) and checks the values it leaves in FCRAM.
Part 2 drives the ARM11-visible devices register by register through QEMU's
qtest protocol, the way libn3ds does: NSPI + CODEC touch/circle pad, I2C + MCU
(RTC, IRQs, HOME/POWER, LCD power), PDC IRQ status and the GIC SPI targets.
No Apple files and no SD image are needed.
"""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
QEMU = os.path.join(ROOT, "build", "qemu", "b", "qemu-system-arm")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
fails = 0


def check(name, got, want):
    global fails
    ok = want(got) if callable(want) else got == want
    if not ok:
        fails += 1
    fmt = (lambda v: "0x%08x" % v) if isinstance(got, int) else str
    print("  %-4s %-46s %s%s" % ("ok" if ok else "FAIL", name, fmt(got),
                                "" if ok or callable(want) else "  (want %s)" % fmt(want)))


def cross():
    c = os.environ.get("CROSS")
    if c:
        return c
    r = subprocess.run([os.path.join(ROOT, "scripts", "setup-toolchain.sh")],
                       capture_output=True, text=True)
    m = re.search(r"cross compiler: (\S*)gcc ", r.stdout)
    return m.group(1) if m else "arm-none-eabi-"


def build_firm(tmp):
    """arm9test.c -> a FIRM with one ARM9 section at 0x08001000, no ARM11 code."""
    cc = cross()
    elf, binf, firm = (os.path.join(tmp, n) for n in ("t.elf", "t.bin", "t.firm"))
    ld = os.path.join(tmp, "t.ld")
    open(ld, "w").write("SECTIONS { . = 0x08001000; .text : { *(.text.start) *(.text*) *(.rodata*) }"
                        " .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n")
    subprocess.run([cc + "gcc", "-mcpu=arm946e-s", "-marm", "-O1", "-ffreestanding", "-nostdlib",
                    "-Wl,-T," + ld, "-Wl,--no-warn-rwx-segments", "-o", elf,
                    os.path.join(HERE, "arm9test.c")], check=True)
    subprocess.run([cc + "objcopy", "-O", "binary", elf, binf], check=True)
    code = open(binf, "rb").read()
    code += b"\0" * (-len(code) % 0x200)
    hdr = bytearray(0x200)
    hdr[0:4] = b"FIRM"
    struct.pack_into("<II", hdr, 8, 0, 0x08001000)          # ARM11 entry 0, ARM9 entry
    struct.pack_into("<IIII", hdr, 0x40, 0x200, 0x08001000, len(code), 0)
    open(firm, "wb").write(bytes(hdr) + code)
    return firm


class Hmp:
    def __init__(self, path):
        self.path = path

    def __call__(self, cmd, wait=0.2):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(self.path)
        time.sleep(0.05)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        out = b""
        try:
            out = s.recv(65536)
        except socket.timeout:
            pass
        s.close()
        return ANSI.sub("", out.decode(errors="replace"))

    def words(self, addr, n):
        out = self("xp/%dwx 0x%x" % (n, addr))
        w = []
        for line in out.splitlines():
            m = re.match(r"^\s*[0-9a-f]{8,16}: (.*)$", line.strip())
            if m:
                w += [int(x, 16) for x in m.group(1).split()]
        return w


def wait_sock(path):
    for _ in range(100):
        if os.path.exists(path):
            return
        time.sleep(0.05)
    sys.exit("QEMU did not come up")


def part1(tmp, firm):
    print("ARM9 devices (arm9test.c under TCG)")
    sd = os.path.join(tmp, "sd.img")
    with open(sd, "wb") as f:
        f.truncate(64 << 20)
    mon = os.path.join(tmp, "mon")
    p = subprocess.Popen([QEMU, "-M", "nintendo3ds", "-kernel", firm, "-display", "none",
                          "-serial", "none", "-monitor", "unix:%s,server,nowait" % mon,
                          "-drive", "file=%s,if=sd,format=raw" % sd],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_sock(mon)
    hmp = Hmp(mon)
    r = []
    for _ in range(40):
        time.sleep(0.25)
        r = hmp.words(0x20001000, 22)
        if len(r) == 22 and r[21] == 0x454E4F44:
            break
    hmp("quit")
    p.wait(timeout=10)
    if len(r) != 22:
        sys.exit("could not read the result block")
    check("test ran to completion", r[21], 0x454E4F44)
    check("CFG9_SDMMCCTL at entry (Luma)", r[1], 0x0200)
    check("TMIO idle STAT0/STAT1", r[2], 0x208004A0)
    check("CFG9_SYSPROT9/11", r[3], 0x0103)
    check("FIFO32 status bits read-only", r[4], 0x1802)
    check("CMD0 with SD clock stopped -> timeout+RESP_END", r[5], 0x20C004A1)
    check("  ERR_STATUS: response timeout", r[17], 0x00010000)
    check("CMD0 on port 1 (eMMC) -> timeout", r[6], 0x20C00001)
    check("CMD0 on port 0", r[7], 0x208004A1)
    check("CMD8", r[8], 0x208004A1)
    check("  R7", r[9], 0x000001AA)
    check("slot power off: DAT3 detect drops", r[10], 0x208000A0)
    check("  CMD8 -> timeout", r[11], 0x20C000A1)
    check("IRQ_IF cleared by writing 1s", r[12], 0)
    check("TMIO IRQ latched in IRQ_IF bit 16", r[13], 0x00010000)
    check("timer 0 counts (/1024)", r[14], lambda v: 0 < v < 0x10000)
    check("timer 0 CNT readback", r[15], 0x83)
    check("timer 1 overflow -> IRQ_IF bit 9", r[16], 0x00000200)
    check("timer 2 counts timer 1 overflows", r[18], lambda v: v > 0)
    check("timer 1 reloads to 0xFF00", r[19], lambda v: 0xFF00 <= v <= 0xFFFF)
    check("ARM9 IRQs delivered via boot9 vector", r[20], lambda v: v > 0)


class QTest:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rw")

    def cmd(self, line):
        self.f.write(line + "\n")
        self.f.flush()
        while True:
            r = self.f.readline().strip()
            if not r.startswith("IRQ "):
                assert r.startswith("OK"), (line, r)
                return r.split()[1:]

    def rd(self, size, addr):
        return int(self.cmd("read%s 0x%x" % (size, addr))[0], 16)

    def wr(self, size, addr, val):
        self.cmd("write%s 0x%x 0x%x" % (size, addr, val))

    def step(self, ns):
        self.cmd("clock_step %d" % ns)


# ---- libn3ds-style drivers on top of qtest --------------------------------
CFG11_SPI_CNT, NSPI2, I2C2, PDC0, GICD = 0x101401C0, 0x10142800, 0x10144000, 0x10400400, 0x17E01000


def spi_xfer(q, tx, rxlen):
    """tsc.c spi_xfer: send, then receive, in one chip-select window."""
    if tx:
        q.wr("l", NSPI2 + 8, len(tx))
        q.wr("l", NSPI2, 0xA005)                  # EN | DIR_S | CS0 | 16 MHz
        for i in range(0, len(tx), 4):
            q.wr("l", NSPI2 + 0xC, int.from_bytes(tx[i:i + 4].ljust(4, b"\0"), "little"))
        assert not q.rd("l", NSPI2) & 0x8000
    rx = b""
    if rxlen:
        q.wr("l", NSPI2 + 8, rxlen)
        q.wr("l", NSPI2, 0x8005)
        for i in range(0, rxlen, 4):
            rx += q.rd("l", NSPI2 + 0xC).to_bytes(4, "little")
        assert not q.rd("l", NSPI2) & 0x8000
    q.wr("b", NSPI2 + 4, 0)
    return rx[:rxlen]


def cdc_write(q, page, reg, val):
    spi_xfer(q, bytes([0, page]), 0)
    spi_xfer(q, bytes([reg << 1, val]), 0)


def cdc_read(q, page, reg, n=1):
    spi_xfer(q, bytes([0, page]), 0)
    return spi_xfer(q, bytes([reg << 1 | 1]), n)


def adc(q):
    raw = cdc_read(q, 0xFB, 1, 0x34)
    w = struct.unpack(">26H", raw)
    return w[0:5], w[5:10], w[10:18], w[18:26]


def i2c_byte(q, data, flags):
    q.wr("b", I2C2, data)
    q.wr("b", I2C2 + 1, 0xC0 | flags)             # EN | IRQ_EN | send
    cnt = q.rd("b", I2C2 + 1)
    assert not cnt & 0x80
    return bool(cnt & 0x10)


def i2c_read(q, dev, reg, n):
    if not (i2c_byte(q, dev, 2) and i2c_byte(q, reg, 0) and i2c_byte(q, dev | 1, 2)):
        q.wr("b", I2C2 + 1, 0xC5)                 # error + stop
        return None
    out = []
    for i in range(n):
        q.wr("b", I2C2 + 1, 0xE0 | (0x10 if i < n - 1 else 0x01))
        out.append(q.rd("b", I2C2))
    return bytes(out)


def i2c_write(q, dev, reg, data):
    ok = i2c_byte(q, dev, 2) and i2c_byte(q, reg, 0)
    for i, b in enumerate(data):
        ok = ok and i2c_byte(q, b, 1 if i == len(data) - 1 else 0)
    return ok


def bcd(b):
    return (b >> 4) * 10 + (b & 15)


def part2(tmp, firm):
    print("ARM11-visible devices (qtest)")
    mon, qt = os.path.join(tmp, "mon2"), os.path.join(tmp, "qt")
    p = subprocess.Popen([QEMU, "-M", "nintendo3ds", "-accel", "qtest", "-kernel", firm,
                          "-display", "none", "-serial", "none",
                          "-monitor", "unix:%s,server,nowait" % mon,
                          "-qtest", "unix:%s,server=on,wait=off" % qt],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_sock(qt)
    wait_sock(mon)
    q, hmp = QTest(qt), Hmp(mon)
    MS = 1000000

    # --- GIC: two-CPU distributor, SPI targets reset to none
    check("GICD_TYPER: 2 CPUs", (q.rd("l", GICD + 4) >> 5) & 7, 1)
    check("GICD_ITARGETSR[42] at reset", q.rd("b", GICD + 0x800 + 42), 0)
    q.wr("b", GICD + 0x800 + 42, 1)
    check("GICD_ITARGETSR[42] after writing 1", q.rd("b", GICD + 0x800 + 42), 1)

    # --- PDC: Luma handoff state needs screen-init; here PDC is off. Enable it.
    q.wr("l", PDC0 + 0x74, 0x00010501)
    q.wr("l", PDC0 + 0x78, 0x00070000)
    q.step(20 * MS)
    check("PDC0 SWAP: only V status after a frame", q.rd("l", PDC0 + 0x78) >> 16, 2)
    q.wr("l", PDC0 + 0x78, 0x00020001)
    check("  V acknowledged, next fb = 1", q.rd("l", PDC0 + 0x78), 0x00000001)
    q.step(20 * MS)
    check("  current fb follows at VBlank", q.rd("l", PDC0 + 0x78) & 0x11, 0x11)
    q.wr("l", PDC0 + 0x74, 0x00010401)             # unmask H
    q.step(1 * MS)
    check("PDC0 H status once H IRQs are unmasked", (q.rd("l", PDC0 + 0x78) >> 16) & 1, 1)
    q.wr("l", PDC0 + 0x74, 0x00010501)
    q.wr("l", PDC0 + 0x78, 0x00070000)

    # --- NSPI + CODEC, the sequence of boot/arm9/tsc.c
    check("CODEC before CFG11_SPI_CNT: bus floats", cdc_read(q, 0x67, 0x25)[0], 0xFF)
    q.wr("w", CFG11_SPI_CNT, 7)
    check("CFG11_SPI_CNT readback", q.rd("w", CFG11_SPI_CNT), 7)
    q.wr("l", NSPI2 + 0x18, 1)
    q.wr("l", NSPI2 + 0x1C, 7)
    check("touch ADC stopped before init (0x67:0x26 bit1)", cdc_read(q, 0x67, 0x26)[0] & 2, 2)
    for reg, val in ((0x24, 0x98), (0x26, 0x00), (0x25, 0x43), (0x24, 0x18), (0x17, 0x43),
                     (0x19, 0x69), (0x1B, 0x80), (0x27, 0x11), (0x26, 0xEC), (0x24, 0x18),
                     (0x25, 0x53)):
        cdc_write(q, 0x67, reg, val)
    cdc_write(q, 0x67, 0x26, cdc_read(q, 0x67, 0x26)[0] | 0x80)
    cdc_write(q, 0x67, 0x24, cdc_read(q, 0x67, 0x24)[0] & 0x7F)
    cdc_write(q, 0x67, 0x25, (cdc_read(q, 0x67, 0x25)[0] & ~0x3C) | 0x10)
    check("0x67:0x25 readback", cdc_read(q, 0x67, 0x25)[0], 0x53)
    check("0x67:0x26: enabled, new data", cdc_read(q, 0x67, 0x26)[0], 0xEC)
    check("multi-byte read auto-increments (0x24..0x27)", cdc_read(q, 0x67, 0x24, 4).hex(),
          "1853ec11")
    check("NSPI transfer-end status", q.rd("l", NSPI2 + 0x1C), 1)
    tx, ty, cy, cx = adc(q)
    check("pen up: touchX bit 12", tx[0] & 0x1000, 0x1000)
    check("circle pad centred", (cy[0], cx[0]), (2048, 2048))
    hmp("mouse_move -1000 -1000")
    hmp("mouse_move 200 360")                      # bottom screen (160,120)
    hmp("mouse_button 1")
    tx, ty, cy, cx = adc(q)
    check("pen down at bottom (160,120): x px", tx[4] * 320 // 4096, 160)
    check("  y px", ty[4] * 240 // 4096, 120)
    check("  5 identical conversions", len(set(tx)) + len(set(ty)), 2)
    hmp("mouse_move 159 119")                      # bottom-right corner (319,239)
    tx, ty, cy, cx = adc(q)
    check("corner (319,239)", (tx[0] * 320 // 4096, ty[0] * 240 // 4096), (319, 239))
    hmp("mouse_move 1 0")                          # x 360: right of the panel
    check("outside the panel: pen up", adc(q)[0][0] & 0x1000, 0x1000)
    hmp("mouse_button 0")
    hmp("sendkey i 500")                           # held until the clock is stepped
    tx, ty, cy, cx = adc(q)
    check("circle pad up (key i): cpadY", cy[0], 2048 + 1200)
    q.step(1100 * MS)                              # release + the queue's trailing delay
    hmp("sendkey l 500")
    tx, ty, cy, cx = adc(q)
    check("circle pad right (key l): cpadX inverted", (cy[7], cx[7]), (2048, 2048 - 1200))
    q.step(1100 * MS)

    # --- I2C + MCU
    check("I2C2: no device at 0x78 -> NACK", i2c_byte(q, 0x78, 2), False)
    q.wr("b", I2C2 + 1, 0xC5)
    check("MCU version regs 0/1", i2c_read(q, 0x4A, 0x00, 2).hex(), "1338")
    t = i2c_read(q, 0x4A, 0x30, 7)
    now = time.gmtime()
    check("MCU RTC (BCD s m h dow d m y) ~ host UTC", t.hex(),
          lambda _: (bcd(t[6]), bcd(t[5]), bcd(t[4]), bcd(t[2])) ==
          (now.tm_year % 100, now.tm_mon, now.tm_mday, now.tm_hour) and bcd(t[1]) == now.tm_min)
    i2c_write(q, 0x4A, 0x34, bytes([0x24, 0x12, 0x11]))      # 24 Dec 2011
    t = i2c_read(q, 0x4A, 0x30, 7)
    check("RTC set to 2011-12-24, weekday derived (6 = Sat)", t[3:].hex(), "06241211")
    i2c_write(q, 0x4A, 0x18, bytes([0x00, 0xFF, 0xFF, 0x00]))
    check("MCU IRQ mask readback", i2c_read(q, 0x4A, 0x18, 4).hex(), "00ffff00")
    check("MCU IRQ status idle", i2c_read(q, 0x4A, 0x10, 4).hex(), "00000000")
    i2c_write(q, 0x4A, 0x22, bytes([0x2A]))                   # LCDs + both backlights on
    check("LCD_PWR -> IRQ bits 25/27/29", i2c_read(q, 0x4A, 0x10, 4).hex(), "0000002a")
    check("  IRQ status clears on read", i2c_read(q, 0x4A, 0x10, 4).hex(), "00000000")
    i2c_write(q, 0x4A, 0x22, bytes([0x01]))                   # LCDs off
    check("EX_HW_STAT after LCD off", i2c_read(q, 0x4A, 0x0F, 1)[0], 0x0A)
    i2c_read(q, 0x4A, 0x10, 4)
    q.wr("b", GICD + 0x800 + 113, 1)                          # IRQ 113 -> CPU 0, enabled;
    q.wr("l", GICD + 0x100 + 12, 1 << 17)                     # distributor off: stays pending
    hmp("sendkey h 300")
    check("HOME press (key h) -> MCU IRQ bit 2", i2c_read(q, 0x4A, 0x10, 1)[0], 0x04)
    check("  MCU IRQ line pending in the GIC (113)", (q.rd("l", GICD + 0x200 + 12) >> 17) & 1, 1)
    q.step(700 * MS)
    check("HOME release -> bit 3", i2c_read(q, 0x4A, 0x10, 1)[0], 0x08)
    hmp("sendkey p 4000")
    check("POWER press (key p) -> bit 0", i2c_read(q, 0x4A, 0x10, 1)[0], 0x01)
    q.step(3100 * MS)
    check("POWER held 3 s -> bit 1", i2c_read(q, 0x4A, 0x10, 1)[0], 0x02)
    q.step(5000 * MS)
    check("  no further MCU events after release", i2c_read(q, 0x4A, 0x10, 1)[0], 0x00)

    hmp("quit")
    p.wait(timeout=10)


def main():
    if not os.path.exists(QEMU):
        sys.exit("build QEMU first: emu/build-qemu.sh")
    base = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
    tmp = tempfile.mkdtemp(prefix="i3t", dir=base)      # short path: UNIX socket limit
    try:
        firm = build_firm(tmp)
        part1(tmp, firm)
        part2(tmp, firm)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%s" % ("ALL OK" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
