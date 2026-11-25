"""No leftover merge-conflict markers in tracked text files.

A half-resolved rebase once left '>>>>>>> <commit>' lines in docs/re-log.md
on main; this keeps them out of commits."""
import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = re.compile(r"^(<{7} |>{7} |={7}$)")
TEXT = (".md", ".c", ".h", ".py", ".json", ".S", ".s", ".sh", ".txt", ".cfg", ".ld", ".mk")


class NoConflictMarkers(unittest.TestCase):
    def test_tracked_files(self):
        try:
            files = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                                   text=True, check=True).stdout.split("\n")
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("not a git checkout")
        bad = []
        for f in files:
            if not f.endswith(TEXT) and os.path.basename(f) != "Makefile":
                continue
            p = os.path.join(ROOT, f)
            if not os.path.isfile(p):
                continue
            with open(p, errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    if MARK.match(line.rstrip("\n")):
                        bad.append("%s:%d" % (f, n))
        self.assertEqual(bad, [], "merge-conflict markers left in: %s" % ", ".join(bad[:10]))


if __name__ == "__main__":
    unittest.main()
