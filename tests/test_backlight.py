"""Host test of the brightness floor (issue #1): the shared
boot/common/backlight.h used by ios3ds-mcud and the kernel shim. No Apple
files or 3DS hardware needed."""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST = r'''
#include <stdio.h>
#include "backlight.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    /* a slider at zero still lights the screen */
    CHECK(ios_backlight_level(0.0f, 1.0f) == IOS_BL_MIN_LEVEL);
    CHECK(ios_backlight_level(0.05f, 1.0f) == IOS_BL_MIN_LEVEL);
    CHECK(ios_backlight_level(0.5f, 0.2f) == IOS_BL_MIN_LEVEL);
    CHECK(ios_backlight_level(-1.0f, 1.0f) == IOS_BL_MIN_LEVEL);

    /* the rest of the range is untouched */
    CHECK(ios_backlight_level(0.5f, 1.0f) == 500);
    CHECK(ios_backlight_level(1.0f, 1.0f) == 1000);
    CHECK(ios_backlight_level(2.0f, 1.0f) == 1000);

    /* factor 0 is iOS' screen-off (lock): the ARM9's backlight == 1 sentinel */
    CHECK(ios_backlight_level(0.5f, 0.0f) == 0);
    CHECK(ios_backlight_level(0.0f, 0.0f) == 0);

    /* PWM: off stays off (shell close, sleep), lit levels stay readable */
    CHECK(ios_backlight_pwm(0) == 0);
    CHECK(ios_backlight_pwm(1) == BL_PWM_MIN);
    CHECK(ios_backlight_pwm(120) == 0x32);
    CHECK(ios_backlight_pwm(500) == 0x5A);
    CHECK(ios_backlight_pwm(1000) == BL_PWM_MAX);
    CHECK(ios_backlight_pwm(2000) == BL_PWM_MAX);
    CHECK(ios_backlight_pwm(ios_backlight_level(0.0f, 1.0f)) >= BL_PWM_MIN);
    CHECK(ios_backlight_pwm(ios_backlight_level(0.5f, 0.0f)) == 0);

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class BacklightFloor(unittest.TestCase):
    def test_shared_helper(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "test_backlight.c")
            with open(src, "w") as f:
                f.write(TEST)
            exe = os.path.join(td, "test_backlight")
            subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I" + os.path.join(ROOT, "boot", "common"),
                            src, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.strip().endswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
