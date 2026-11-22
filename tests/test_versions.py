"""Offline tests for version selection, patch relocation and recipe overlays.
Synthetic data only: no Apple files or IPSWs are needed."""
import hashlib
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "kernel"))

import mkrootfs  # noqa: E402
import patcher  # noqa: E402
import versions  # noqa: E402


class TestVersions(unittest.TestCase):
    def test_default_has_no_overrides(self):
        info = versions.get()
        self.assertEqual(info["build"], "8C148")
        self.assertIsNone(versions.patch_files())
        self.assertIsNone(versions.recipe_path())
        self.assertEqual(versions.ba_version(), 5)
        self.assertEqual(versions.staging_dir(), os.path.join("build", "staging"))

    def test_registry_knows_old_builds(self):
        for v, build in (("2.1.1", "5F138"), ("2.2", "5G77a"), ("2.2.1", "5H11a"),
                         ("3.1.3", "7E18"), ("4.0", "8A293"), ("4.1", "8B117"),
                         ("4.2.1", "8C148")):
            self.assertEqual(versions.get(v)["build"], build)
        with self.assertRaises(SystemExit):
            versions.get("9.9.9")

    def test_other_version_layout(self):
        self.assertEqual(versions.staging_dir("4.1"), os.path.join("build", "fw-4.1", "staging"))
        pf = versions.patch_files("4.1")
        self.assertEqual(len(pf), 1)
        self.assertTrue(os.path.isfile(pf[0]), pf[0])

    def test_recipe_overlay(self):
        with tempfile.TemporaryDirectory() as td:
            path = versions.recipe_path("4.1", td)
            self.assertEqual(path, os.path.join(td, "recipe.json"))
            with open(path) as f:
                recipe = json.load(f)
            with open(os.path.join(ROOT, "rootfs", "recipe.json")) as f:
                base = json.load(f)
            self.assertEqual(recipe["_file_patches_search"],
                             "offsets replaced by original-byte searches")
            self.assertTrue(all("offset" not in fp and "search" in fp
                                for fp in recipe["file_patches"]))
            self.assertEqual(recipe["write"]["/etc/fstab"], base["write"]["/etc/fstab"])
            # the 4.1 overlay carries symbol-relocated file-patch offsets:
            # inapplicable patches are dropped, the rest use fixed offsets
            self.assertEqual(len(recipe["file_patches"]), 18)
            self.assertTrue(all("offset" in fp and "search" not in fp
                                for fp in recipe["file_patches"]))


class TestPatcher(unittest.TestCase):
    def test_fixed_va(self):
        img = bytearray(b"\x00" * 0x100)
        p = {"name": "fixed", "va": "0x80000010", "orig": "11223344", "new": "44332211"}
        self.assertEqual(patcher.locate(img, 0x80000000, p), 0x80000010)
        applied = patcher.apply_all(img, 0x80000000, "/nonexistent", files=[])
        self.assertEqual(applied, [])
        with open(os.path.join(tempfile.gettempdir(), "ios3ds-test-patch.json"), "w") as f:
            json.dump([p], f)
        fn = f.name
        try:
            img[0x10:0x14] = bytes.fromhex(p["orig"])
            applied = patcher.apply_all(img, 0x80000000, fn, files=[fn])
            self.assertEqual(applied[0]["va"], 0x80000010)
            self.assertEqual(bytes(img[0x10:0x14]), bytes.fromhex(p["new"]))
        finally:
            os.remove(fn)

    def test_find_unique_and_bounded(self):
        img = bytearray(b"abcPATTERNdef" + b"PATTERN")
        base = 0x80000000
        p = {"name": "found", "find": {"orig": b"PATTERN".hex(), "from": "0x80000000",
                                       "to": hex(base + 13)},
             "orig": b"PATTERN".hex(), "new": "X" * 14}
        self.assertEqual(patcher.locate(img, base, p), base + 3)
        p2 = dict(p, name="ambiguous")
        p2["find"] = {"orig": b"PATTERN".hex()}
        with self.assertRaises(ValueError):
            patcher.locate(img, base, p2)
        p3 = dict(p, name="with-context")
        p3["find"] = {"orig": b"PATTERN".hex(), "before": b"abc".hex(), "after": b"def".hex()}
        self.assertEqual(patcher.locate(img, base, p3), base + 3)


class TestFilePatches(unittest.TestCase):
    def test_offset_and_search(self):
        data = b"aaaORIGINALbbb"
        fp = {"offset": "0x3", "path": "/f", "orig": b"ORIGINAL".hex(), "new": "00" * 8}
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp.pop("offset")
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp["orig_sha256"] = hashlib.sha256(b"ORIGINAL").hexdigest()
        del fp["orig"]
        self.assertEqual(mkrootfs.resolve_patch_offset(data, fp), 3)
        fp["orig_sha256"] = hashlib.sha256(b"nope").hexdigest()
        with self.assertRaises(SystemExit):
            mkrootfs.resolve_patch_offset(data, fp)


if __name__ == "__main__":
    unittest.main()
