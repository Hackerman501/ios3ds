import bisect, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fwlib.macho import MachO
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_b = open(os.path.join(_ROOT, "build", "staging", "kernelcache.macho"), "rb").read()
_m = MachO(_b)
_so, _n, _str, _ = _m.symtab
_syms = []
for i in range(_n):
    strx, typ, sect, desc, val = struct.unpack_from("<IBBhI", _b, _so + 12 * i)
    if val:
        _syms.append((val & ~1, _b[_str + strx:_b.index(b"\0", _str + strx)].decode()))
_syms.sort()
_addrs = [s[0] for s in _syms]
def name(a):
    i = bisect.bisect_right(_addrs, a & ~1) - 1
    return "%s+%#x" % (_syms[i][1], (a & ~1) - _syms[i][0]) if i >= 0 else "?"
