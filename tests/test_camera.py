"""Host test of the camera path's pure logic: the shim's CAM FIFO drain
(kernel/shim/cam.c built with -DCAM_HOST_TEST) against a model CAM block, and
the YUV conversion / test pattern (boot/common/cam_pattern.h); see
tools/ios3ds-camera/tests/test_cam.c and docs/camera.md. Also checks the
rootfs recipe's camera entries."""
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class CameraDrain(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory() as td:
            exe = os.path.join(td, "test_cam")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DCAM_HOST_TEST",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            os.path.join(ROOT, "kernel", "shim", "cam.c"),
                            os.path.join(ROOT, "tools", "ios3ds-camera", "tests", "test_cam.c"),
                            "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


class CameraRootfs(unittest.TestCase):
    def test_recipe(self):
        with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
            r = json.load(f)
        self.assertEqual(r["add"]["build/rootfs-add/System/Library/MediaCapture/vcd.mediacapture"],
                         "/System/Library/MediaCapture/vcd.mediacapture")
        caps = r["plist_merge"]["/System/Library/CoreServices/SpringBoard.app/N72AP.plist"]["capabilities"]
        self.assertTrue(caps["still-camera"] and caps["front-facing-camera"])
        with open(os.path.join(ROOT, "rootfs", "files", "camera", "com.apple.coremedia.plist"), "rb") as f:
            prefs = plistlib.load(f)
        self.assertIs(prefs["vcd"], True)
        self.assertNotIn("ios3dsPattern", prefs)        # the shipped image uses the real cameras
        with open(os.path.join(ROOT, "rootfs", "files", "camera", "AVCapture.plist"), "rb") as f:
            av = plistlib.load(f)
        for mode, port in (("AVCaptureMode_PhotoCapture", "PortTypeBack"),
                           ("AVCaptureMode_FrontFacingPhotoCapture", "PortTypeFront")):
            lso = av[mode]["LiveSourceOptions"]
            self.assertEqual(lso["VideoPort"], port)
            for k in ("Sensor", "Capture", "Preview"):
                self.assertEqual((lso[k]["Width"], lso[k]["Height"], lso[k]["PixelFormatType"]),
                                 (320, 240, "BGRA"), mode + " " + k)

    def test_plist_merge(self):
        import mkrootfs
        d = {"capabilities": {"wifi": True, "still-camera": False}, "x": 1}
        mkrootfs.merge_dict(d, {"capabilities": {"still-camera": True}})
        self.assertEqual(d, {"capabilities": {"wifi": True, "still-camera": True}, "x": 1})


if __name__ == "__main__":
    unittest.main()
