"""Offline tests of scripts/carryover.py (user data carry-over on update)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

HFS = os.path.join(ROOT, "build", "tools", "hfsplus")
MKFS = shutil.which("mkfs.hfsplus")


def have_tools():
    return os.path.exists(HFS) and MKFS is not None


@unittest.skipUnless(have_tools(), "need build/tools/hfsplus and mkfs.hfsplus")
class CarryoverTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.mkdtemp()
        self.old = os.path.join(self.td, "old.hfs")
        self.new = os.path.join(self.td, "new.hfs")
        for p in (self.old, self.new):
            with open(p, "wb") as f:
                f.truncate(4 * 1024 * 1024)
            subprocess.run([MKFS, "-v", "T", p], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        import carryover
        self.carryover = carryover
        self.hfs = HFS
        # populate old image
        for d in ("/private", "/private/var", "/private/var/mobile",
                  "/private/var/mobile/Library",
                  "/private/var/mobile/Library/Preferences",
                  "/private/var/Keychains",
                  "/private/var/preferences",
                  "/private/var/preferences/SystemConfiguration"):
            subprocess.run([self.hfs, self.old, "mkdir", d], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(os.path.join(self.td, "pref.plist"), "w") as f:
            f.write("hello")
        subprocess.run([self.hfs, self.old, "add",
                        os.path.join(self.td, "pref.plist"),
                        "/private/var/mobile/Library/Preferences/pref.plist"], check=True)
        subprocess.run([self.hfs, self.old, "chmod", "0644",
                        "/private/var/mobile/Library/Preferences/pref.plist"], check=True)
        with open(os.path.join(self.td, "kc.key"), "w") as f:
            f.write("secret")
        subprocess.run([self.hfs, self.old, "add",
                        os.path.join(self.td, "kc.key"),
                        "/private/var/Keychains/kc.key"], check=True)
        subprocess.run([self.hfs, self.old, "chmod", "0600",
                        "/private/var/Keychains/kc.key"], check=True)

    def tearDown(self):
        shutil.rmtree(self.td, ignore_errors=True)

    def _ls_mode(self, img, path):
        r = subprocess.run([self.hfs, img, "ls", os.path.dirname(path)],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 7 and parts[-1] == os.path.basename(path):
                return parts[0]
        return None

    def test_files_survive_with_mode(self):
        self.carryover.carry_over(self.old, self.new, self.hfs, backup=False, verbose=False)
        self.assertEqual(self._ls_mode(self.new, "/private/var/mobile/Library/Preferences/pref.plist"), "100644")
        self.assertEqual(self._ls_mode(self.new, "/private/var/Keychains/kc.key"), "100600")
        # content
        r = subprocess.run([self.hfs, self.new, "cat",
                            "/private/var/mobile/Library/Preferences/pref.plist"],
                           capture_output=True)
        self.assertEqual(r.stdout, b"hello")

    def test_missing_old_warns_and_continues(self):
        bogus = os.path.join(self.td, "nope.hfs")
        # should not raise
        self.carryover.carry_over(bogus, self.new, self.hfs, backup=False, verbose=False)

    def test_fresh_env_skips(self):
        os.environ["IOS3DS_FRESH"] = "1"
        try:
            # remove any previously carried file by using a fresh new image
            with open(self.new, "wb") as f:
                f.truncate(4 * 1024 * 1024)
            subprocess.run([MKFS, "-v", "T", self.new], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.carryover.carry_over(self.old, self.new, self.hfs, backup=False, verbose=False)
            self.assertIsNone(self._ls_mode(self.new, "/private/var/mobile/Library/Preferences/pref.plist"))
        finally:
            os.environ.pop("IOS3DS_FRESH", None)


if __name__ == "__main__":
    unittest.main()
