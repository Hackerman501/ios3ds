"""Boot-time work (docs/boot-perf.md): the kernel patches' encodings, the
rootfs recipe's boot settings, and emu/bootprof.py's analysis on a
synthetic log."""
import json
import os
import plistlib
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "emu"))


class BootPatches(unittest.TestCase):
    def setUp(self):
        self.p = {x["name"]: x for x in json.load(open(os.path.join(ROOT, "kernel", "patches-boot.json")))}

    def test_wdt_wait_returns_null(self):
        p = self.p["wdt-no-pmu-wait"]
        self.assertEqual(bytes.fromhex(p["orig"]), struct.pack("<H", 0x4798))     # blx r3
        self.assertEqual(bytes.fromhex(p["new"]), struct.pack("<H", 0x2000))      # movs r0, #0

    def test_clock_controller_literal(self):
        p = self.p["clkctl-voltage-quiet"]
        self.assertEqual(struct.unpack("<I", bytes.fromhex(p["orig"]))[0], 0x802e4815)
        self.assertEqual(p["new"], "@shim+0x28@")       # ARM entry: blx to an even address
        entry = open(os.path.join(ROOT, "kernel", "shim", "entry.S")).read()
        self.assertIn("b shim_quiet_clk_function       @ +0x28", entry)


class Recipe(unittest.TestCase):
    def setUp(self):
        self.r = json.load(open(os.path.join(ROOT, "rootfs", "recipe.json")))

    def test_localtime_is_a_symlink(self):
        # a regular file made CoreFoundation scan all of /usr/share/zoneinfo in
        # every process that asked for the time zone (~340 md0 reads each)
        self.assertEqual(self.r["symlink"]["/private/var/db/timezone/localtime"], "/usr/share/zoneinfo/UTC")
        self.assertNotIn("/private/var/db/timezone/localtime", self.r["add"].values())

    def test_no_mail_autolaunch(self):
        sb = self.r["write"]["/private/var/mobile/Library/Preferences/com.apple.springboard.plist"]
        self.assertTrue(plistlib.loads(sb.encode())["SBDisableMailAutoLaunch"])

    def test_diag_jobs_are_gated(self):
        for plist in ("rootfs/files/com.ios3ds.sample.plist", "tools/ios3ds-gl/test/com.ios3ds.gltest.plist",
                      "tools/ios3ds-audio/tone/com.ios3ds.tone.plist", "tools/ios3ds-diag/com.ios3ds.iobusy.plist"):
            d = plistlib.load(open(os.path.join(ROOT, plist), "rb"))
            self.assertEqual(d["ProgramArguments"][0], "/usr/libexec/ios3ds-diag", plist)
            self.assertNotEqual(d.get("KeepAlive"), True, plist)    # an exit 0 must not respawn it


class BootprofAnalysis(unittest.TestCase):
    def test_report(self):
        import bootprof
        with tempfile.TemporaryDirectory() as td:
            log = [
                "[boot] 0.500 s: SD mounted, ARM11 ready",
                "[boot] 2.000 s: XNU started",
                "[2.500] BSD root: md0, major 2, minor 0",
                "[blk] 2.600 r 100+64 buf 65536 f800000c3 500 us",
                "[blk] 2.601 r 164+64 buf 65536 fc3 500 us",
                "[3.000] com.apple.launchd 1\t*** launchd[1] has started up. ***",
                "[blk] 3.100 r 900+8 buf 4096 f80000101 100 us",
                "[4.000] [input] connected to SpringBoard's HID event system",
                "[4.100] [input] mailbox up: touch ok",
                "[10.000] [input] down 46,314",
                "[blk] 10.200 r 2000+8 buf 4096 f80000043 100 us",
                "[blk] 10.900 r 3000+64 buf 32768 f800000c3 300 us",
            ]
            open(os.path.join(td, "log.txt"), "w").write("\n".join(log) + "\n")
            qp = []
            for i in range(1, 60):          # 250 ms lines: busy until 5 s and 10-11.5 s
                t = i * 0.25
                busy = 90 if t <= 5 or 10 < t <= 11.5 else 2
                qp.append("[qprof] a9t=%.3f t=%.3f a11 %d%% a9 100%% md0 0%% md0+a11idle 0%%" % (t, t, busy))
            open(os.path.join(td, "qprof.txt"), "w").write("\n".join(qp) + "\n")
            json.dump([["boot quiet", 9.9], ["tap Settings", 9.9], ["Settings quiet", 14.0]],
                      open(os.path.join(td, "events.json"), "w"))
            bootprof.analyse(td, show=False)
            rep = json.load(open(os.path.join(td, "report.json")))
        phases = {p["phase"]: p for p in rep["boot"]}
        self.assertEqual(phases["launchd"]["chunks"], 2)
        self.assertEqual(phases["launchd"]["xnu_requests"], 1)
        self.assertEqual(phases["launchd"]["read_KiB"], 64)
        self.assertEqual(phases["SpringBoard HID up"]["meta_KiB"], 4)
        app = rep["apps"][0]
        self.assertEqual(app["app"], "Settings")
        self.assertEqual(app["xnu_requests"], 2)
        self.assertAlmostEqual(app["md0_burst_s"], 0.9, places=2)
        self.assertAlmostEqual(app["settled_s"], 1.5, places=2)


if __name__ == "__main__":
    unittest.main()
