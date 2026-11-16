#!/usr/bin/env python3
"""Reproducible kernel facts for docs/research.md, from a staged kernelcache.macho.

  scripts/kscan.py build/staging/kernelcache.macho

Prints: Mach-O layout, the boot_args fields read by the entry stub, every
ARM-mode CP15 / SMC / MCRR instruction in the kernel and kexts, and the
sites known to be problematic on ARM11 MPCore. Thumb code (most of pexpert
and the kexts) can't issue CP15 ops on ARMv6, so an ARM-mode scan is complete
for coprocessor access, modulo literal pools that happen to decode as MCR/MRC.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fwlib.macho import MachO  # noqa: E402

# CP15 encodings that exist on ARM1176 but are absent/different on ARM11 MPCore.
# (see docs/research.md section 3 for sources)
SUSPECT = {
    ("c15", "c2", 4): "ARM1176 Peripheral Port Memory Remap - not on MPCore",
    ("c6", "c0", 2): "IFAR - check against MPCore TRM",
    ("c1", "c0", 1): "ACTLR - bit layout differs (MPCore bit5 = SMP)",
}


def scan(blob, base_va):
    hits = collections.defaultdict(list)
    for i in range(0, len(blob) - 3, 4):
        w = struct.unpack_from("<I", blob, i)[0]
        if w >> 28 != 0xE:
            continue
        va = base_va + i
        if (w & 0x0F000F10) == 0x0E000F10:
            op = "MRC" if (w >> 20) & 1 else "MCR"
            key = (op, (w >> 21) & 7, "c%d" % ((w >> 16) & 15), "c%d" % (w & 15), (w >> 5) & 7)
            hits[key].append(va)
        elif (w & 0x0FE00F00) == 0x0C400F00:
            hits[("MCRR/MRRC", 0, "-", "c%d" % (w & 15), 0)].append(va)
        elif (w & 0x0FFFFFF0) == 0x01600070:
            hits[("SMC", 0, "-", "-", 0)].append(va)
    return hits


def main():
    b = open(sys.argv[1], "rb").read()
    m = MachO(b)
    print(m.summary())
    entry_off = m.va_to_off(m.entry)
    print("\nentry %#x (file %#x); first words: %s" % (
        m.entry, entry_off, " ".join("%08x" % x for x in struct.unpack_from("<8I", b, entry_off))))
    for label, (seg, sect) in (("kernel", ("__TEXT", "__text")), ("kexts", ("__PRELINK_TEXT", "__text"))):
        s = m.section(seg, sect)
        hits = scan(b[s["offset"]: s["offset"] + s["size"]], s["addr"])
        print("\n== %s ARM-mode coprocessor/secure instructions" % label)
        for key in sorted(hits):
            op, opc1, crn, crm, opc2 = key
            note = SUSPECT.get((crn, crm, opc2), "") if opc1 == 0 else "(opc1!=0: probably data)"
            sites = hits[key]
            print("  %-9s p15,%d,%-3s,%-3s,%d  x%-4d %s %s" % (
                op, opc1, crn, crm, opc2, len(sites),
                " ".join("%#x" % v for v in sites[:4]) + (" ..." if len(sites) > 4 else ""), note))


if __name__ == "__main__":
    main()
