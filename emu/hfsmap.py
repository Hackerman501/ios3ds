#!/usr/bin/env python3
"""Which files did md0 read? Maps the 512-byte sectors in a bootprof blk.csv
to HFS+ files of rootfs.hfs (data and resource forks, catalog/extents/
attributes B-trees, journal), per phase.

  python3 emu/hfsmap.py build/sd/ios3ds/rootfs.hfs build/emu/bootprof [--top 25]

Reads report.json for the phase boundaries. docs/boot-perf.md.
"""
import argparse
import bisect
import collections
import csv
import json
import os
import struct


def fork(b, off):
    size, _, blocks = struct.unpack_from(">QII", b, off)
    ext = [struct.unpack_from(">II", b, off + 16 + 8 * i) for i in range(8)]
    return size, blocks, [e for e in ext if e[1]]


class HFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        vh = self.read(1024, 512)
        assert vh[:2] in (b"H+", b"HX"), "not HFS+"
        self.attrs, self.jib = struct.unpack_from(">II", vh, 4)[0], struct.unpack_from(">I", vh, 12)[0]
        self.bs = struct.unpack_from(">I", vh, 40)[0]
        self.special = {}
        for name, off in (("[allocation bitmap]", 112), ("[extents B-tree]", 192), ("[catalog B-tree]", 272),
                          ("[attributes B-tree]", 352)):
            self.special[name] = fork(vh, off)
        self.overflow = {}
        self.overflow = self.read_extents_overflow()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def fork_bytes(self, ext):
        return b"".join(self.read(s * self.bs, c * self.bs) for s, c in ext)

    def leaves(self, ext):
        data = self.fork_bytes(ext)
        node_size = struct.unpack_from(">H", data, 32)[0]
        first_leaf = struct.unpack_from(">I", data, 24)[0]
        n = first_leaf
        while n:
            node = data[n * node_size:(n + 1) * node_size]
            flink, _, kind, _, nrec = struct.unpack_from(">IIbBH", node, 0)
            offs = [struct.unpack_from(">H", node, node_size - 2 * (i + 1))[0] for i in range(nrec)]
            for o in offs:
                yield node, o
            n = flink

    def read_extents_overflow(self):
        out = collections.defaultdict(list)
        size, blocks, ext = self.special["[extents B-tree]"]
        if not ext:
            return out
        for node, o in self.leaves(ext):
            klen, ftype, _, fid, start = struct.unpack_from(">HBBII", node, o)
            d = o + 2 + klen
            out[(fid, ftype)] += [e for e in (struct.unpack_from(">II", node, d + 8 * i) for i in range(8)) if e[1]]
        return out

    def files(self):
        """[(label, [(start_block, count)...])] for every fork, plus the special files"""
        names, items = {}, []
        size, blocks, ext = self.special["[catalog B-tree]"]
        ext = ext + self.overflow.get((4, 0), [])
        for node, o in self.leaves(ext):
            klen, parent, nlen = struct.unpack_from(">HIH", node, o)
            name = node[o + 8:o + 8 + 2 * nlen].decode("utf-16-be", errors="replace")
            d = o + 2 + klen
            d += d & 1
            rtype = struct.unpack_from(">h", node, d)[0]
            if rtype in (1, 2):
                cnid = struct.unpack_from(">I", node, d + 8)[0]
                names[cnid] = (parent, name)
            if rtype == 2:
                for ftype, off in ((0, 88), (0xFF, 168)):
                    fsize, fblocks, fext = fork(node, d + off)
                    fext = fext + self.overflow.get((cnid, ftype), [])
                    if fext:
                        items.append((cnid, ftype, fext))

        def path(cnid):
            parts = []
            while cnid in names and cnid != 2:
                parent, name = names[cnid]
                parts.append(name)
                cnid = parent
            return "/" + "/".join(reversed(parts))
        out = [(path(c) + (" (rsrc)" if t else ""), e) for c, t, e in items]
        for name, (size, blocks, ext) in self.special.items():
            out.append((name, ext + self.overflow.get(({"[extents B-tree]": 3, "[catalog B-tree]": 4,
                                                         "[attributes B-tree]": 8,
                                                         "[allocation bitmap]": 6}[name], 0), [])))
        if self.attrs & 0x2000 and self.jib:
            jib = self.read(self.jib * self.bs, 52)
            joff, jsize = struct.unpack_from(">QQ", jib, 36)
            out.append(("[journal]", [(joff // self.bs, jsize // self.bs)]))
            out.append(("[journal info]", [(self.jib, 1)]))
        out.append(("[volume header]", [(0, 1)]))
        return out


def build_map(img):
    h = HFS(img)
    spb = h.bs // 512
    iv = []
    for label, ext in h.files():
        for s, c in ext:
            iv.append((s * spb, (s + c) * spb, label))
    iv.sort()
    return iv, [i[0] for i in iv]


def label_of(iv, starts, sec):
    i = bisect.bisect_right(starts, sec) - 1
    if i >= 0 and iv[i][0] <= sec < iv[i][1]:
        return iv[i][2]
    return "[free/unknown]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("dir")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    iv, starts = build_map(a.image)
    rep = json.load(open(os.path.join(a.dir, "report.json")))
    phases = [(p["phase"], p["t0"], p["t1"]) for p in rep["boot"]]
    phases += [("app " + p["app"], p["t0"], p["t1"]) for p in rep["apps"]]
    rows = list(csv.DictReader(open(os.path.join(a.dir, "blk.csv"))))
    out = []
    for name, t0, t1 in phases:
        per, reqs, total = collections.Counter(), collections.Counter(), 0
        for r in rows:
            t = float(r["t"])
            if not (t0 <= t < t1) or r["op"] != "r":
                continue
            lba, n = int(r["lba"]), int(r["sectors"])
            reqs[label_of(iv, starts, lba)] += 1
            for s in range(lba, lba + n, 8):      # 4 KiB granules are enough
                per[label_of(iv, starts, s)] += min(8, lba + n - s)
            total += n
        if not total:
            continue
        out.append("## %s (%.1f-%.1f s): %d KiB read\n" % (name, t0, t1, total // 2))
        out.append("| KiB | chunks | file |\n|---|---|---|")
        for lab, secs in per.most_common(a.top):
            out.append("| %d | %d | %s |" % (secs // 2, reqs[lab], lab))
        out.append("")
    with open(os.path.join(a.dir, "files.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
