"""Apply the kernel patch recipes (kernel/patches.json) to the flat kernel image.

Each patch: {"name", "va", "orig": "hex bytes", "new": "hex bytes", "why",
             "enabled": true}
'orig' must match exactly, or the build stops: a different kernelcache can
never be silently mis-patched. Hex strings are in memory byte order (e.g.
"00f020e3" is the ARM word 0xe320f000). 'new' may contain @sym+0xOFF@ tokens,
replaced by the little-endian word sym+OFF (symbols come from mkboot, e.g. the
shim's virtual address).
"""
import json
import re
import struct

TOKEN = re.compile(r"@([a-z_]+)\+0x([0-9a-fA-F]+)@")


def _expand(hexstr, symbols):
    def rep(m):
        if m.group(1) not in symbols:
            raise KeyError("unknown patch symbol %s" % m.group(1))
        return struct.pack("<I", symbols[m.group(1)] + int(m.group(2), 16)).hex()
    return TOKEN.sub(rep, hexstr.replace(" ", ""))


def locate(img, base_va, p):
    """Return the VA of a patch. `va` keeps the original fixed-address
    behaviour; `find` looks up the verified original bytes (optionally with
    context) in a bounded window and requires exactly one hit."""
    if "va" in p:
        return int(p["va"], 16) if isinstance(p["va"], str) else p["va"]
    f = p.get("find")
    if not f:
        raise ValueError("patch %s: neither va nor find" % p["name"])
    pat = bytes.fromhex(f.get("orig", p["orig"]).replace(" ", ""))
    before = bytes.fromhex(f.get("before", "").replace(" ", ""))
    after = bytes.fromhex(f.get("after", "").replace(" ", ""))
    needle = before + pat + after
    start = int(f.get("from", "0"), 0) if isinstance(f.get("from", "0"), str) else f.get("from", 0)
    end = int(f.get("to", "0"), 0) if isinstance(f.get("to", "0"), str) else f.get("to", 0)
    lo = max(0, start - base_va) if start else 0
    hi = min(len(img), end - base_va) if end else len(img)
    hits = []
    pos = lo
    while True:
        i = bytes(img[pos:hi]).find(needle)
        if i < 0:
            break
        hits.append(pos + i + len(before))
        pos += i + 1
    if len(hits) != 1:
        raise ValueError("patch %s: %d matches for pattern %s in [%#x,%#x)"
                         % (p["name"], len(hits), needle.hex(), start, end))
    return base_va + hits[0]


def apply_all(img, base_va, path, symbols=None, disabled=(), files=None):
    """path: kernel/patches.json; every kernel/patches-*.json next to it is
    applied too (one file per subsystem keeps parallel work conflict-free).
    `files` overrides that list for a non-default iOS version."""
    import glob
    import os
    recipes = []
    if files is None:
        files = [path] + sorted(glob.glob(os.path.join(os.path.dirname(path), "patches-*.json")))
    for fn in files:
        with open(fn) as f:
            recipes += json.load(f)
    applied = []
    for p in recipes:
        if not p.get("enabled", True) or p["name"] in disabled:
            continue
        va = locate(img, base_va, p)
        orig = bytes.fromhex(p["orig"].replace(" ", ""))
        new = bytes.fromhex(_expand(p["new"], symbols or {}))
        if p.get("branch"):
            # new = ARM 'b' from this patch's va to a shim symbol mkboot resolved
            # (the 4-byte 'new' placeholder is overwritten); used where no fixed
            # shim jump-table slot is free (kernel/patches-sandbox.json)
            tgt = (symbols or {})[p["branch"]]
            delta = tgt - (va + 8)
            if delta % 4:
                raise ValueError("patch %s: unaligned branch to %s" % (p["name"], p["branch"]))
            new = struct.pack("<I", 0xEA000000 | ((delta // 4) & 0xFFFFFF))
        if len(orig) != len(new):
            raise ValueError("patch %s: orig/new length differ" % p["name"])
        off = va - base_va
        cur = bytes(img[off:off + len(orig)])
        if cur != orig:
            raise ValueError("patch %s @%#x: expected %s, found %s (wrong kernelcache?)"
                             % (p["name"], va, orig.hex(), cur.hex()))
        img[off:off + len(new)] = new
        applied.append({"name": p["name"], "va": va, "why": p.get("why", "")})
    return applied
