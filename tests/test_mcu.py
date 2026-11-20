"""Host tests of the MCU drivers (docs/mcu.md): the ARM9 driver
(boot/arm9/mcu.c) against a fake MCU, the gyroscope driver (boot/arm9/gyro.c)
and its rad/s conversion and axis map against fake chips, and the encodings
of the MCU kernel patches (kernel/patches-mcu.json)."""
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class Arm9McuDriver(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            exe = os.path.join(td, "test_mcu")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            "-I" + os.path.join(ROOT, "boot", "arm9"),
                            os.path.join(ROOT, "boot", "arm9", "mcu.c"),
                            os.path.join(ROOT, "tools", "ios3ds-mcu", "tests", "test_mcu.c"),
                            "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class Arm9GyroDriver(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            exe = os.path.join(td, "test_gyro")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            "-I" + os.path.join(ROOT, "boot", "arm9"),
                            os.path.join(ROOT, "boot", "arm9", "gyro.c"),
                            os.path.join(ROOT, "tools", "ios3ds-mcu", "tests", "test_gyro.c"),
                            "-lm", "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


def thumb_ldr_pc(va, hw):
    """ldr rX,[pc,#imm] at va -> literal address"""
    return ((va + 4) & ~3) + (hw & 0xFF) * 4


class McuPatches(unittest.TestCase):
    """The patches jump into the shim with 'ldr r3,[pc,#0]': the literal must
    be the word the patch carries, i.e. the encodings depend on alignment."""

    def setUp(self):
        with open(os.path.join(ROOT, "kernel", "patches-mcu.json")) as f:
            self.p = {x["name"]: x for x in json.load(f)}

    def _new(self, name):
        p = self.p[name]
        new = bytes.fromhex(p["new"].split("@")[0])
        return int(p["va"], 16), new, p

    def test_lengths_and_tokens(self):
        for p in self.p.values():
            nbytes = len(p["new"].split("@")[0]) // 2 + (4 if "@shim+" in p["new"] else 0)
            self.assertEqual(nbytes, len(bytes.fromhex(p["orig"])), p["name"])

    def test_function_entry_jumps(self):
        for name in ("pe-get-gmt", "pe-set-gmt"):
            va, new, _ = self._new(name)
            ldr, bx = struct.unpack("<HH", new[:4])
            self.assertEqual(ldr & 0xFF00, 0x4B00, name)           # ldr r3, [pc, #imm]
            self.assertEqual(bx, 0x4718, name)                       # bx r3
            self.assertEqual(thumb_ldr_pc(va, ldr), va + 4, name)    # literal right after

    def test_halt_call_site(self):
        va, new, _ = self._new("pe-halt-mcu")
        adds, ldr, b = struct.unpack("<HHH", new[:6])
        self.assertEqual(adds, 0x1C29)                               # adds r1, r5, #0
        self.assertEqual(ldr & 0xFF00, 0x4B00)
        lit = thumb_ldr_pc(va + 2, ldr)
        self.assertEqual(lit, va + 6)                                # literal = the token
        self.assertEqual(b & 0xF800, 0xE000)                         # b
        imm = b & 0x7FF
        self.assertEqual(va + 4 + 4 + imm * 2, va + 10)              # skips the literal
        self.assertEqual(va + 10, 0x801C9900)                        # onto the original blx r3

    def test_shim_slots(self):
        with open(os.path.join(ROOT, "kernel", "shim", "entry.S")) as f:
            src = f.read()
        for off, fn in ((0x30, "shim_mcu_get_gmt"), (0x34, "shim_mcu_wait_rtc"),
                        (0x38, "shim_mcu_halt_restart"), (0x3C, "shim_mcu_set_gmt")):
            self.assertIn("b %s" % fn, src)
            self.assertRegex(src, r"b %s\s+@ \+0x%X" % (fn, off))
        tokens = {p["name"]: p["new"].split("@")[1] for p in self.p.values() if "@" in p["new"]}
        self.assertEqual(tokens["pe-get-gmt"], "shim+0x30")
        self.assertEqual(tokens["iortc-wait-mcu"], "shim+0x34")
        self.assertEqual(tokens["pe-halt-mcu"], "shim+0x38")
        self.assertEqual(tokens["pe-set-gmt"], "shim+0x3c")

    def test_gyro_vtable(self):
        """AppleLIS331DLH vtable 0x806200d0: +0x3b4 getPrimaryUsage -> an ARM
        'mov r0,#9' (even address), +0x3c0 dispatch -> the ARM shim slot +0x2C"""
        u = self.p["gyro-usage"]
        self.assertEqual(int(u["va"], 16), 0x806200d0 + 0x3b4)
        self.assertEqual(struct.unpack("<I", bytes.fromhex(u["orig"]))[0], 0x8061b978)
        tgt = struct.unpack("<I", bytes.fromhex(u["new"]))[0]
        self.assertEqual(tgt, 0x8034433c)
        self.assertEqual(tgt & 3, 0)
        d = self.p["gyro-dispatch"]
        self.assertEqual(int(d["va"], 16), 0x806200d0 + 0x3c0)
        self.assertEqual(struct.unpack("<I", bytes.fromhex(d["orig"]))[0], 0x80546771)
        self.assertEqual(d["new"], "@shim+0x2c@")
        with open(os.path.join(ROOT, "kernel", "shim", "entry.S")) as f:
            self.assertRegex(f.read(), r"b shim_gyro_dispatch\s+@ \+0x2C")

    def test_gyro_dyld_patch(self):
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            fp = [p for p in json.load(f)["file_patches"] if "getProviders" in p["why"]]
        self.assertEqual(len(fp), 1)
        o, n = (struct.unpack("<I", bytes.fromhex(fp[0][k]))[0] for k in ("orig", "new"))
        self.assertEqual(n, o | 0x40)           # only the gyro provider bit

    def test_commpage_branch(self):
        """beq -> b: the unconditional branch must land on the same epilogue"""
        p = self.p["commpage-gtod-off"]
        va = int(p["va"], 16)
        beq, = struct.unpack("<H", bytes.fromhex(p["orig"]))
        b, = struct.unpack("<H", bytes.fromhex(p["new"]))
        self.assertEqual(beq & 0xFF00, 0xD000)
        self.assertEqual(b & 0xF800, 0xE000)
        t_beq = va + 4 + ((beq & 0xFF) ^ 0x80) * 2 - 0x100
        t_b = va + 4 + ((b & 0x7FF) ^ 0x400) * 2 - 0x800
        self.assertEqual(t_beq, t_b)
        self.assertEqual(t_b, 0x8006AECC)       # commpage_set_timestamp's epilogue


if __name__ == "__main__":
    unittest.main()
