#!/usr/bin/env python3
"""Build sd:/ios3ds/wifi-fw.bin from YOUR console's NWM system module.

The Old 3DS Wi-Fi chip (Atheros AR6014) has no usable firmware in ROM: the
host must upload Nintendo's Xtensa firmware over SDIO after every power-up.
ios3ds does not ship it. This tool cuts the firmware blocks out of the NWM
module's .code (title 00040130-00002d02, see GBATEK "3DS Files - Module NWM")
that you extract from your own console, and packs them into the small
container the ARM9 driver reads (boot/arm9/wifi/ar6k.c, docs/wifi.md).

  nwmfw.py NWM.code [-o wifi-fw.bin] [--main type4|type1] [--list]

The input is NWM's ExeFS .code, decompressed or still BLZ-compressed (both
are accepted). GodMode9: CTRNAND title/00040130/00002d02/content/<id>.app ->
"NCCH image options..." -> "Extract .code".

How the blocks are found (GBATEK): the ARM11 code loads the block addresses
from a literal pool that contains the constants 00524C00h (main/stub-data
destination), 0053FE18h (database destination) and 00527000h (stub-code
destination). Two pool layouts are known ("newer" and "older"); both are
handled. Every result is checked against the sizes GBATEK lists as constant
(stub data 38h, stub code 316h, database 1E8h) before anything is written.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import struct
import sys

FW_MAGIC = 0x46573349           # 'I3WF', boot/arm9/wifi/ar6k.c
FW_VERSION = 1
FW_STUB_DATA, FW_STUB_CODE, FW_DATABASE, FW_MAIN_LZ = 1, 2, 3, 4
CHIP_ID_AR6014 = 0x0D000001
MAIN_DST, DB_DST, CODE_DST = 0x00524C00, 0x0053FE18, 0x00527000
POOL_MAGIC2 = 0x000003ED
EXPECTED = {"stub_data": 0x38, "stub_code": 0x316, "database": 0x1E8}
TEXT_BASES = (0x00100000, 0x00010000)   # 3DS .text base; GBATEK writes 10000h


class FwError(Exception):
    pass


# --------------------------------------------------------------------------
# BLZ ("bottom LZ", 3DS ExeFS .code) - decoder as in ctrtool's lzss.c

def blz_footer_ok(data):
    if len(data) < 8:
        return False
    tb, extra = struct.unpack_from("<II", data, len(data) - 8)
    bottom, top = tb >> 24, tb & 0xFFFFFF
    return 8 <= bottom <= 0x0B and bottom <= top <= len(data) and extra < 0x1000000


def blz_decompress(data):
    if not blz_footer_ok(data):
        raise FwError("not a BLZ-compressed .code (bad footer)")
    tb, extra = struct.unpack_from("<II", data, len(data) - 8)
    bottom, top = tb >> 24, tb & 0xFFFFFF
    out = bytearray(len(data) + extra)
    out[:len(data)] = data
    index = len(data) - bottom
    stop = len(data) - top
    o = len(out)
    while index > stop:
        index -= 1
        control = data[index]
        for _ in range(8):
            if index <= stop or o <= 0:
                break
            if control & 0x80:
                if index < 2:
                    raise FwError("BLZ: truncated back-reference")
                index -= 2
                seg = data[index] | data[index + 1] << 8
                size = (seg >> 12) + 3
                off = (seg & 0x0FFF) + 2
                for _ in range(size):
                    if o + off >= len(out) or o <= 0:
                        raise FwError("BLZ: back-reference out of range")
                    out[o - 1] = out[o + off]
                    o -= 1
            else:
                index -= 1
                o -= 1
                out[o] = data[index]
            control = (control << 1) & 0xFF
    return bytes(out)


# --------------------------------------------------------------------------
# literal pools

def _u32(c, off):
    if off < 0 or off + 4 > len(c):
        return None
    return struct.unpack_from("<I", c, off)[0]


def find_pools(c):
    """Return a list of dicts: {layout, main: {type: (start, end)}, stub_data,
    stub_code, database} with *addresses* (not yet file offsets)."""
    found = []
    for i in range(0, len(c) - 3, 4):
        if _u32(c, i) != MAIN_DST:
            continue
        w = lambda d: _u32(c, i + d)  # noqa: E731
        # newer large pool: main.dst at +14h of the pool
        if w(4) == POOL_MAGIC2 and w(0x28) == DB_DST and w(0x34) == CODE_DST:
            found.append({
                "layout": "newer",
                "main": {"type1": (w(0x0C), w(0x08)), "type4": (w(0x14), w(0x10)),
                         "type5": (w(0x1C), w(0x18))},
                "database": (w(0x24), w(0x20)),
                "stub_code": (w(0x30), w(0x2C)),
                "stub_data": (w(0x3C), w(0x38)),
            })
        # older large pool: main.dst at +18h, type5; type1/type4 from its tail
        # (+38h..+44h, 3EDh at +48h) or from the older small pool (main.dst,
        # t5, t1, t4, 3EDh at +1Ch)
        elif w(0x0C) == DB_DST and w(0x18) == CODE_DST:
            ent = {
                "layout": "older",
                "main": {"type5": (w(0x08), w(0x04))},
                "database": (w(0x14), w(0x10)),
                "stub_code": (w(0x20), w(0x1C)),
                "stub_data": (w(0x28), w(0x24)),
            }
            if w(0x48) == POOL_MAGIC2:
                ent["main"]["type1"] = (w(0x3C), w(0x38))
                ent["main"]["type4"] = (w(0x44), w(0x40))
                found.append(ent)
                continue
            for j in range(0, len(c) - 0x1F, 4):
                if _u32(c, j) == MAIN_DST and _u32(c, j + 0x1C) == POOL_MAGIC2:
                    ent["main"]["type1"] = (_u32(c, j + 0x10), _u32(c, j + 0x0C))
                    ent["main"]["type4"] = (_u32(c, j + 0x18), _u32(c, j + 0x14))
                    break
            found.append(ent)
    return found


def _resolve(c, pool, base):
    """Turn a pool's address ranges into file slices; None if they do not fit."""
    out = {}
    for key in ("stub_data", "stub_code", "database"):
        s, e = pool[key]
        if s is None or e is None or not (base <= s < e <= base + len(c)):
            return None
        out[key] = (s - base, e - base)
    mains = {}
    for t, (s, e) in pool["main"].items():
        if s is not None and e is not None and base <= s < e <= base + len(c):
            mains[t] = (s - base, e - base)
    out["main"] = mains
    return out


def extract(code, main_type="type4"):
    """Locate the blocks. Returns (parts dict of bytes, info dict)."""
    tried = [("raw", code)]
    if blz_footer_ok(code):
        tried.append(("blz", None))
    for how, c in tried:
        if c is None:
            c = blz_decompress(code)
        for pool in find_pools(c):
            for base in TEXT_BASES:
                r = _resolve(c, pool, base)
                if not r:
                    continue
                sizes = {k: r[k][1] - r[k][0] for k in EXPECTED}
                if sizes != EXPECTED:
                    continue
                if main_type not in r["main"]:
                    raise FwError("main firmware %s not referenced by this NWM (has: %s)"
                                  % (main_type, ", ".join(sorted(r["main"])) or "none"))
                ms, me = r["main"][main_type]
                parts = {k: c[r[k][0]:r[k][1]] for k in EXPECTED}
                parts["main"] = c[ms:me]
                info = {"input": how, "layout": pool["layout"], "text_base": base,
                        "mains": {t: e - s for t, (s, e) in r["main"].items()}}
                return parts, info
    raise FwError("no NWM firmware pool found: is this NWM's .code (00040130-00002d02)?")


def pack(parts):
    """The container read by ar6k.c fw_check(): header, then the blocks."""
    order = [(FW_STUB_DATA, MAIN_DST, parts["stub_data"]),
             (FW_STUB_CODE, CODE_DST, parts["stub_code"]),
             (FW_DATABASE, DB_DST, parts["database"]),
             (FW_MAIN_LZ, MAIN_DST, parts["main"])]
    hdr_size = 16 + 8 * 16
    blob = bytearray()
    entries = []
    for kind, dst, data in order:
        off = hdr_size + len(blob)
        entries.append(struct.pack("<IIII", kind, dst, off, len(data)))
        blob += data
        blob += b"\0" * (-len(blob) % 4)
    hdr = struct.pack("<IIII", FW_MAGIC, FW_VERSION, len(order), CHIP_ID_AR6014)
    hdr += b"".join(entries) + b"\0" * 16 * (8 - len(order))
    return bytes(hdr + blob)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("code", help="NWM .code (decompressed or BLZ)")
    ap.add_argument("-o", "--out", default="wifi-fw.bin")
    ap.add_argument("--main", default="type4", choices=("type1", "type4", "type5"),
                    help="main firmware variant (default type4; GBATEK: type1 of newer NWM hangs)")
    ap.add_argument("--list", action="store_true", help="only show what was found")
    a = ap.parse_args(argv)
    with open(a.code, "rb") as f:
        code = f.read()
    try:
        parts, info = extract(code, a.main)
    except FwError as e:
        print("nwmfw: %s" % e, file=sys.stderr)
        return 1
    print("nwmfw: %s .code, %s pool, text base %#x; main variants: %s" % (
        info["input"], info["layout"], info["text_base"],
        ", ".join("%s %#x bytes" % kv for kv in sorted(info["mains"].items()))))
    if a.list:
        return 0
    blob = pack(parts)
    with open(a.out, "wb") as f:
        f.write(blob)
    print("nwmfw: wrote %s (%d bytes, main %s). Copy it to sd:/ios3ds/wifi-fw.bin;"
          " do not share it (Nintendo code)." % (a.out, len(blob), a.main))
    return 0


if __name__ == "__main__":
    sys.exit(main())
