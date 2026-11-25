"""Host test of the accelerometer -> UIDeviceOrientation mapping (issue #1).

The 180 degree case (UIDeviceOrientationPortraitUpsideDown = 2) used to be
suspected of never being reported. The shim's axis map is now shared with this
test, which checks every QEMU MCU pose. No Apple files are needed."""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST = r'''
#include <stdio.h>
#include "accel_axes.h"

#define G 16384
/* emu/qemu/hw/arm/nintendo3ds.c mcu_poses[0..5]: pose 2 is upside down */
static const int16_t poses[6][3] = {
    { G, 0, 0 }, { 0, -G, 0 }, { -G, 0, 0 }, { 0, G, 0 }, { 0, 0, G }, { 0, 0, -G }
};
/* SpringBoard's com.apple.springboard.rawOrientation, as logged by mcud in
   build/emu-rot2 (Safari open, one QOM pose at a time; 0 = unknown). */
static const int orientation[6] = { 1, 4, 2, 3, 5, 6 };

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    int seen[7] = { 0 };
    for (int i = 0; i < 6; i++) {
        int v[3];
        ios_accel_axes(poses[i], v);
        int got = ios_device_orientation(v);
        CHECK(got == orientation[i]);
        if (got >= 0 && got < 7) seen[got]++;
        printf("pose %d: raw %6d %6d %6d -> iOS %6d %6d %6d -> orientation %d\n",
               i, poses[i][0], poses[i][1], poses[i][2], v[0], v[1], v[2], got);
    }
    /* the 180 degree case is a distinct reported orientation, not a duplicate
       of portrait or of a landscape pose */
    CHECK(seen[2] == 1);
    CHECK(seen[1] == 1 && seen[3] == 1 && seen[4] == 1);
    CHECK(ios_device_orientation((const int[]){ 0, G, 0 }) == 2);
    CHECK(ios_device_orientation((const int[]){ 0, 0, G }) == 5);
    CHECK(ios_device_orientation((const int[]){ 0, 0, 0 }) == 0);

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class OrientationMap(unittest.TestCase):
    def test_all_six_poses(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "test_orientation.c")
            with open(src, "w") as f:
                f.write(TEST)
            exe = os.path.join(td, "test_orientation")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            src, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)

    def test_shim_uses_shared_map(self):
        with open(os.path.join(ROOT, "tools", "ios3ds-mcu", "shim", "mcu.c")) as f:
            src = f.read()
        self.assertIn("ios_accel_axes(raw, v)", src)
        # the old inline signs must not reappear next to it
        self.assertNotIn("v[1] = -b->acc[0]", src)

    def test_qemu_upside_down_pose(self):
        with open(os.path.join(ROOT, "emu", "qemu", "hw", "arm", "nintendo3ds.c")) as f:
            src = f.read()
        self.assertIn("{ -MCU_ACC_1G, 0, 0 },", src)
        self.assertIn("right side up: iOS upside down", src)


if __name__ == "__main__":
    unittest.main()
