#!/usr/bin/env python3
"""Nearest exported kernel symbol for addresses: scripts/ksym.py 0x8001d4ba ..."""
import bisect, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fwlib.macho import MachO
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
b = open(os.path.join(ROOT, "build", "staging", "kernelcache.macho"), "rb").read()
m = MachO(b)
symoff, nsyms, stroff, _ = m.symtab
syms = []
for i in range(nsyms):
    strx, typ, sect, desc, val = struct.unpack_from("<IBBhI", b, symoff + 12 * i)
    if val:
        syms.append((val & ~1, b[stroff + strx:b.index(b"\0", stroff + strx)].decode()))
syms.sort()
addrs = [s[0] for s in syms]
for a in sys.argv[1:]:
    v = int(a, 16) & ~1
    i = bisect.bisect_right(addrs, v) - 1
    print("%s  %s+0x%x" % (a, syms[i][1], v - syms[i][0]) if i >= 0 else a)
