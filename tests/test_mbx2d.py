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

    def test_benign_failure_strings_are_silenced(self):
        # 8C148 QuartzCore still probes MBX2D with the env vars set (checked
        # in QEMU), so the two log formats in the cache are emptied. The
        # fallback path itself is unchanged.
        with open(os.path.join(ROOT, "rootfs", "recipe.json"), encoding="utf-8") as f:
            recipe = json.load(f)
        patches = {p.get("offset"): p for p in recipe["file_patches"]}
        for offset, length in (("0x7707cc", 34), ("0x47bb470", 42)):
            self.assertIn(offset, patches)
            p = patches[offset]
            self.assertEqual(p["path"], "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6")
            self.assertEqual(len(bytes.fromhex(p["orig"])), length)
            self.assertEqual(bytes.fromhex(p["new"]), b"\0" * length)


if __name__ == "__main__":
    unittest.main()
