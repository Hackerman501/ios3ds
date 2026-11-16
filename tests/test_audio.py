"""Host test of the shim's PL080/CSND/I2C emulation (kernel/shim/audio.c built
with -DAUD_HOST_TEST against tools/ios3ds-audio/tests/test_audio.c)."""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class AudioEmulation(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            exe = os.path.join(td, "test_audio")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DAUD_HOST_TEST",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            "-I" + os.path.join(ROOT, "kernel", "shim"),
                            os.path.join(ROOT, "kernel", "shim", "audio.c"),
                            os.path.join(ROOT, "tools", "ios3ds-audio", "tests", "test_audio.c"),
                            "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
