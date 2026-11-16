"""Minimal 32-bit Mach-O reader (enough to summarise / locate a kernelcache)."""
import struct

MH_MAGIC = 0xFEEDFACE
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_UUID = 0x1B
CPU_TYPE_ARM = 12


class MachO:
    def __init__(self, blob: bytes):
        self.blob = blob
        (magic, self.cputype, self.cpusubtype, self.filetype, ncmds, sizeofcmds,
         self.flags) = struct.unpack_from("<7I", blob, 0)
        if magic != MH_MAGIC:
            raise ValueError("not a 32-bit LE Mach-O (magic %08x)" % magic)
        self.segments = []  # dicts
        self.entry = None
        self.uuid = None
        self.symtab = None
        off = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<2I", blob, off)
            if cmd == LC_SEGMENT:
                name = blob[off + 8 : off + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flg = \
                    struct.unpack_from("<8I", blob, off + 24)
                sects = []
                so = off + 56
                for _ in range(nsects):
                    sn = blob[so : so + 16].rstrip(b"\0").decode()
                    sg = blob[so + 16 : so + 32].rstrip(b"\0").decode()
                    addr, ssize, soff = struct.unpack_from("<3I", blob, so + 32)
                    sects.append({"sect": sn, "seg": sg, "addr": addr, "size": ssize, "offset": soff})
                    so += 68
                self.segments.append({"name": name, "vmaddr": vmaddr, "vmsize": vmsize,
                                      "fileoff": fileoff, "filesize": filesize, "sections": sects})
            elif cmd == LC_UNIXTHREAD:
                flavor, count = struct.unpack_from("<2I", blob, off + 8)
                regs = struct.unpack_from("<%dI" % count, blob, off + 16)
                self.entry = regs[15]  # ARM_THREAD_STATE: r0-r12, sp, lr, pc, cpsr
            elif cmd == LC_UUID:
                self.uuid = blob[off + 8 : off + 24].hex()
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from("<4I", blob, off + 8)
            off += size

    def segment(self, name):
        return next((s for s in self.segments if s["name"] == name), None)

    def section(self, seg, sect):
        for s in self.segments:
            for x in s["sections"]:
                if x["seg"] == seg and x["sect"] == sect:
                    return x
        return None

    def va_to_off(self, va):
        for s in self.segments:
            if s["vmaddr"] <= va < s["vmaddr"] + s["filesize"]:
                return s["fileoff"] + va - s["vmaddr"]
        return None

    def summary(self):
        lines = ["cputype=%d subtype=%d filetype=%d flags=%#x entry=%#x uuid=%s" % (
            self.cputype, self.cpusubtype, self.filetype, self.flags, self.entry or 0, self.uuid)]
        for s in self.segments:
            lines.append("  %-16s vm %#010x-%#010x  file %#09x+%#09x" % (
                s["name"], s["vmaddr"], s["vmaddr"] + s["vmsize"], s["fileoff"], s["filesize"]))
            for x in s["sections"]:
                lines.append("      %-18s %#010x %#9x" % (x["sect"], x["addr"], x["size"]))
        return "\n".join(lines)
