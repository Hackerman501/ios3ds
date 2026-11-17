#!/usr/bin/env python3
"""Minimal GDB remote-protocol client for QEMU's gdbstub.

Arm GNU gdb 14.2 has no Python and the host gdb has no ARM target, so the
scripted traces talk to QEMU (-s -S) directly. ARM register numbers follow
QEMU's arm core XML: r0-r15 = 0..15, cpsr = 25.
"""
import socket


class RSP:
    def __init__(self, host="127.0.0.1", port=1234):
        self.s = socket.create_connection((host, port))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.bps = set()
        self.buf = b""
        self.ack = True
        self.cmd("qSupported:swbreak+;hwbreak+")
        if self.cmd("QStartNoAckMode") == "OK":
            self.ack = False
        self.cmd("?")

    def _recv_packet(self):
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                data = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                if self.ack:
                    self.s.sendall(b"+")
                return data.decode("latin1")
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("gdbstub closed")
            self.buf += chunk

    def cmd(self, data):
        raw = data.encode("latin1")
        pkt = b"$" + raw + b"#%02x" % (sum(raw) & 0xFF)
        self.s.sendall(pkt)
        if self.ack:
            while True:
                if not self.buf:
                    self.buf += self.s.recv(65536)
                c, self.buf = self.buf[:1], self.buf[1:]
                if c == b"+":
                    break
        return self._recv_packet()

    @staticmethod
    def _le(hexstr):
        return int.from_bytes(bytes.fromhex(hexstr), "little")

    def reg(self, n):
        return self._le(self.cmd("p%x" % n))

    def pc(self):
        return self.reg(15)

    def cpsr(self):
        return self.reg(25)

    def regs(self):
        return [self.reg(i) for i in range(16)] + [self.cpsr()]

    def read(self, addr, n):
        r = self.cmd("m%x,%x" % (addr, n))
        if r.startswith("E"):
            raise IOError("read %08x: %s" % (addr, r))
        return bytes.fromhex(r)

    def u32(self, addr):
        return int.from_bytes(self.read(addr, 4), "little")

    def hbreak(self, addr):
        assert self.cmd("Z1,%x,4" % addr) == "OK"
        self.bps.add(addr)

    def hbreak_del(self, addr):
        self.cmd("z1,%x,4" % addr)
        self.bps.discard(addr)

    def cont(self):
        """Continue; like gdb, step off a breakpoint we are sitting on first."""
        pc = self.pc()
        if pc in self.bps:
            self.cmd("z1,%x,4" % pc)
            self.cmd("s")
            self.cmd("Z1,%x,4" % pc)
        return self.cmd("c")

    def step(self):
        return self.cmd("s")

    def threads(self):
        out, r = [], self.cmd("qfThreadInfo")
        while r.startswith("m"):
            out += r[1:].split(",")
            r = self.cmd("qsThreadInfo")
        return out

    def select(self, tid):
        self.cmd("Hg" + tid)
        self.cmd("Hc" + tid)
