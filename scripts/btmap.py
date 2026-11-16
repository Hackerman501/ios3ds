#!/usr/bin/env python3
"""Annotate XNU panic backtraces (stdin) with kext names / exported symbols."""
import bisect, os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
out = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "kexts.py")], capture_output=True, text=True).stdout
kexts = []
for l in out.splitlines():
    m = re.match(r"(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", l)
    if m and int(m.group(3), 16):
        kexts.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1).replace("com.apple.", "")))
import ksym_lib
for line in sys.stdin:
    line = line.rstrip()
    m = re.search(r"lr: 0x([0-9a-f]{8})", line)
    if m:
        a = int(m.group(1), 16)
        k = [x for x in kexts if x[0] <= a < x[0] + x[1]]
        line += "   " + ("%s+%#x" % (k[0][2], a - k[0][0]) if k else ksym_lib.name(a))
    print(line)
