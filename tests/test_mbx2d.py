"""MBX2D must not be initialised by QuartzCore (docs/gpu.md §8).

The dyld cache RE put both user-visible messages in our build logs:
MBX2D.framework prints "Failed to open connection to MBX" when its
AppleMBXDevice service lookup finds nothing, QuartzCore prints
"Failed to initialized MBX2D driver (1)." when its CA::RenderMBX2D path is
enabled. QuartzCore already continues, but games that reach that path can
crash; disable it in the environment apps inherit from SpringBoard.
"""
import json
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class MBX2DRecipeTest(unittest.TestCase):
    def test_springboard_disables_mbx2d_but_keeps_gl_compositing(self):
        with open(os.path.join(ROOT, "rootfs", "recipe.json"), encoding="utf-8") as f:
            recipe = json.load(f)
        env = recipe["plist_set"]["/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"][
            "EnvironmentVariables"]
        self.assertEqual(env.get("CA_ENABLE_MBX2D"), "0")
        self.assertEqual(env.get("LK_ENABLE_MBX2D"), "0")
        self.assertEqual(env.get("CA_ENABLE_OGL"), "1")
        self.assertEqual(env.get("MBX2D_PAGE_FLIP"), "0")
        self.assertEqual(env.get("IOS3DS_CA"), "1")


if __name__ == "__main__":
    unittest.main()
