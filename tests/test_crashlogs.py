#!/usr/bin/env python3
"""Host tests for scripts/crashlogs.py (image sniffing, hfsplus ls parsing)."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import crashlogs  # noqa: E402
from test_carryover import HFS, have_tools, make_image, hfs  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


class CrashLogsTest(unittest.TestCase):
    def test_wanted_suffixes(self):
        for name in ("AppStore-2026-11-23-123456.plist", "SpringBoard.crash", "kernel.PANIC",
                     "weird.Crash"):
            self.assertTrue(crashlogs.wanted(name), name)
        for name in ("log.txt", "report.plist.old", "CrashReporter", ""):
            self.assertFalse(crashlogs.wanted(name), name)

    def test_parse_ls(self):
        text = "\n".join([
            "CrashReporter: ",
            "040755 501 501 0 11/23/2026 12:00 .",
            "100644 501 501 4321 11/23/2026 12:01 AppStore-2026.crash",
            "000000 0 0 123 11/23/2026 12:01 SpringBoard-2026.plist",
            "Total filesystem size: 691625984, free: 382115840",
        ])
        self.assertEqual(crashlogs.parse_ls(text), [
            ("040755", 0, "."),
            ("100644", 4321, "AppStore-2026.crash"),
            ("000000", 123, "SpringBoard-2026.plist"),
        ])
        self.assertEqual(crashlogs.parse_ls("No such file or directory\n"), [])

    def test_hfs_signature(self):
        with tempfile.TemporaryDirectory() as d:
            hfs = os.path.join(d, "rootfs.hfs")
            with open(hfs, "wb") as f:
                f.write(b"\0" * 0x400 + b"HX" + b"\0" * 10)
            self.assertTrue(crashlogs.is_hfs_image(hfs))
            with open(hfs, "r+b") as f:
                f.seek(0x400)
                f.write(b"H+")
            self.assertTrue(crashlogs.is_hfs_image(hfs))
            with open(hfs, "r+b") as f:
                f.seek(0x400)
                f.write(b"MS")
            self.assertFalse(crashlogs.is_hfs_image(hfs))

    def test_find_reports_recurses(self):
        listing = {
            crashlogs.HFS_PATHS[0]: ["040755 501 501 0 11/23/2026 12:00 Sub",
                                     "100644 501 501 10 11/23/2026 12:00 A.crash",
                                     "100644 501 501 10 11/23/2026 12:00 ignore.txt"],
            crashlogs.HFS_PATHS[0] + "/Sub": ["100644 501 501 10 11/23/2026 12:00 B.plist"],
        }
        seen = []

        def fake_list(hfs, image, path):
            return crashlogs.parse_ls("\n".join(listing.get(path, [])))

        orig = crashlogs.list_dir
        crashlogs.list_dir = fake_list
        try:
            seen = list(crashlogs.find_reports("hfs", "img"))
        finally:
            crashlogs.list_dir = orig
        self.assertEqual([p for p, _ in seen], [
            crashlogs.HFS_PATHS[0] + "/A.crash",
            crashlogs.HFS_PATHS[0] + "/Sub/B.plist",
        ])


@unittest.skipUnless(have_tools(), "need build/tools/hfsplus and mkfs.hfsplus")
class CrashLogsImageTest(unittest.TestCase):
    def test_extract_from_hfs(self):
        with tempfile.TemporaryDirectory(prefix="ios3ds-crashlogs-") as d:
            img = os.path.join(d, "rootfs.hfs")
            make_image(img, 8 * 1024 * 1024)
            for path in ("/private", "/private/var", "/private/var/mobile",
                         "/private/var/mobile/Library", "/private/var/mobile/Library/Logs",
                         "/private/var/mobile/Library/Logs/CrashReporter",
                         "/private/var/mobile/Library/Logs/CrashReporter/Sub"):
                hfs(img, "mkdir", path, check=False)
            src = os.path.join(d, "report.crash")
            with open(src, "w") as f:
                f.write("Incident Identifier: TEST-123\n")
            hfs(img, "add", src, "/private/var/mobile/Library/Logs/CrashReporter/AppStore.crash")
            hfs(img, "add", src, "/private/var/mobile/Library/Logs/CrashReporter/Sub/inner.panic")
            hfs(img, "add", src, "/private/var/mobile/Library/Logs/CrashReporter/not-a-report.txt")
            out = os.path.join(d, "out")
            r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "crashlogs.py"),
                                img, out, "--hfsplus", HFS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            names = sorted(os.listdir(out))
            self.assertEqual(names, ["private__var__mobile__Library__Logs__CrashReporter__AppStore.crash",
                                     "private__var__mobile__Library__Logs__CrashReporter__Sub__inner.panic"])
            with open(os.path.join(out, names[0])) as f:
                self.assertIn("TEST-123", f.read())


if __name__ == "__main__":
    unittest.main()
