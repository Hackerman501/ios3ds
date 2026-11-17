#!/usr/bin/env python3
"""picasm: a small assembler for PICA200 vertex shaders.

Just enough for the ios3ds fixed-function shader, with a syntax that is a
subset of picasso's (https://github.com/devkitPro/picasso), so the same
source can be cross-checked against it:

    ; comment
    .out   NAME SEMANTIC        position | color | texcoord0 | texcoord1 | texcoord2
    .alias NAME REGISTER
    .proc main ... .end
    OP dst[.mask], src1[, src2]

Registers: o0-o15 outputs, v0-v15 inputs, r0-r15 temporaries, c0-c95 float
uniforms. Sources may be negated ("-r0") and swizzled (".xyzw" letters, the
last one repeating). Instructions: add dp3 dp4 mul max min sge slt (two
sources), mov rcp rsq flr ex2 lg2 (one source), nop, end.

    picasm.py shader.vsh -o shader.h [--name pica_ff]

writes a C header with the instruction words, the operand descriptors and
the output map. `assemble()` is the library entry point; `run()` executes
an assembled program on one vertex (used by the tests).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import re
import sys

OPC2 = {"add": 0x00, "dp3": 0x01, "dp4": 0x02, "mul": 0x08, "sge": 0x09, "slt": 0x0A,
        "max": 0x0C, "min": 0x0D}
OPC1 = {"ex2": 0x05, "lg2": 0x06, "flr": 0x0B, "rcp": 0x0E, "rsq": 0x0F, "mov": 0x13}
OPC0 = {"nop": 0x21, "end": 0x22}
COMMUTATIVE = {"add", "dp3", "dp4", "mul", "max", "min"}

# semantic -> (first component id in SH_OUTMAP, components, SH_OUTATTR_CLOCK bit, uses texcoord mode)
SEMANTICS = {
    "position":  (0x00, 4, 0, False),
    "color":     (0x08, 4, 1, False),
    "texcoord0": (0x0C, 2, 8, True),
    "texcoord1": (0x0E, 2, 9, True),
    "texcoord2": (0x16, 2, 10, True),
}
COMP = {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3, "s": 0, "t": 1, "p": 2, "q": 3}


class AsmError(Exception):
    pass


class Program:
    def __init__(self):
        self.code = []          # instruction words
        self.opdesc = []        # operand descriptor words
        self.entry = 0
        self.outputs = []       # (register, semantic)

    @property
    def outmap(self):
        """(total, mask, [7 map words], mode, clock) as the registers want them."""
        maps = [0x1F1F1F1F] * 7
        mask = mode = clock = 0
        for reg, sem in self.outputs:
            first, n, clk, tex = SEMANTICS[sem]
            mask |= 1 << reg
            clock |= 1 << clk
            mode |= int(tex)
            word = maps[reg]
            for i in range(n):
                word = word & ~(0xFF << (8 * i)) | (first + i) << (8 * i)
            maps[reg] = word
            if sem == "position":
                clock |= 1 << 0     # z is used
        return bin(mask).count("1"), mask, maps, mode, clock


def _reg(tok, aliases, dest):
    """Parse '[-]name[.comps]' -> (register number, negate, component list or None)."""
    neg = tok.startswith("-")
    if neg:
        tok = tok[1:]
    name, _, comps = tok.partition(".")
    name = aliases.get(name, name)
    m = re.fullmatch(r"([ovrc])(\d+)", name)
    if not m:
        raise AsmError("unknown register %r" % tok)
    kind, n = m.group(1), int(m.group(2))
    limit = {"o": 16, "v": 16, "r": 16, "c": 96}[kind]
    if n >= limit:
        raise AsmError("register out of range: %r" % tok)
    if dest:
        if kind not in "or" or neg:
            raise AsmError("bad destination %r" % tok)
        num = n if kind == "o" else 0x10 + n
    else:
        if kind == "o":
            raise AsmError("output register used as a source: %r" % tok)
        num = {"v": 0x00, "r": 0x10, "c": 0x20}[kind] + n
    sel = None
    if comps:
        try:
            sel = [COMP[ch] for ch in comps]
        except KeyError:
            raise AsmError("bad components %r" % tok)
        if len(sel) > 4:
            raise AsmError("bad components %r" % tok)
    return num, neg, sel


def _src_field(neg, sel):
    """9-bit source field of an operand descriptor: negate | swizzle << 1."""
    sel = list(sel or [0, 1, 2, 3])
    sel += [sel[-1]] * (4 - len(sel))
    sw = sel[0] << 6 | sel[1] << 4 | sel[2] << 2 | sel[3]
    return int(neg) | sw << 1


def _dest_mask(sel):
    if sel is None:
        return 0xF
    if sorted(set(sel)) != sel:
        raise AsmError("destination components must be distinct and in xyzw order")
    return sum(8 >> c for c in sel)


def assemble(text):
    prog = Program()
    aliases = {}
    in_proc = False
    seen_main = False

    def opdesc_index(value):
        if value not in prog.opdesc:
            prog.opdesc.append(value)
        idx = prog.opdesc.index(value)
        if idx > 127:
            raise AsmError("too many operand descriptors")
        return idx

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        try:
            head, _, rest = line.partition(" ")
            head = head.lower()
            args = [a.strip() for a in rest.split(",")] if rest.strip() else []
            if head == ".out":
                name, sem = rest.split()
                if sem not in SEMANTICS:
                    raise AsmError("unknown output semantic %r" % sem)
                reg = len(prog.outputs)
                if reg >= 7:
                    raise AsmError("too many outputs")
                prog.outputs.append((reg, sem))
                aliases[name] = "o%d" % reg
            elif head == ".alias":
                name, target = rest.split()
                aliases[name] = aliases.get(target, target)
            elif head == ".proc":
                if rest.strip() != "main" or seen_main:
                    raise AsmError("only one procedure, 'main', is supported")
                in_proc = seen_main = True
                prog.entry = len(prog.code)
            elif head == ".end":
                in_proc = False
            elif head.startswith("."):
                raise AsmError("unsupported directive %s" % head)
            elif not in_proc:
                raise AsmError("instruction outside .proc")
            elif head in OPC0:
                prog.code.append(OPC0[head] << 26)
            elif head in OPC1:
                if len(args) != 2:
                    raise AsmError("%s takes dst, src" % head)
                d, _, dsel = _reg(args[0], aliases, True)
                s1, n1, sel1 = _reg(args[1], aliases, False)
                desc = _dest_mask(dsel) | _src_field(n1, sel1) << 4
                prog.code.append(OPC1[head] << 26 | d << 21 | s1 << 12 | opdesc_index(desc))
            elif head in OPC2:
                if len(args) != 3:
                    raise AsmError("%s takes dst, src1, src2" % head)
                d, _, dsel = _reg(args[0], aliases, True)
                a = _reg(args[1], aliases, False)
                b = _reg(args[2], aliases, False)
                # only the first source field is wide enough for a uniform
                if b[0] >= 0x20:
                    if a[0] >= 0x20 or head not in COMMUTATIVE:
                        raise AsmError("%s: only the first source may be a uniform" % head)
                    a, b = b, a
                if a[0] < 0x10 and b[0] < 0x10 and a[0] != b[0]:
                    raise AsmError("two different input registers in one instruction")
                desc = _dest_mask(dsel) | _src_field(a[1], a[2]) << 4 | _src_field(b[1], b[2]) << 13
                prog.code.append(OPC2[head] << 26 | d << 21 | a[0] << 12 | b[0] << 7 | opdesc_index(desc))
            else:
                raise AsmError("unknown instruction %r" % head)
        except (AsmError, ValueError) as e:
            raise AsmError("line %d: %s" % (lineno, e)) from None
    if not seen_main:
        raise AsmError("no .proc main")
    if not prog.code or prog.code[-1] >> 26 != OPC0["end"]:
        raise AsmError("main must finish with 'end'")
    if not any(sem == "position" for _, sem in prog.outputs):
        raise AsmError("no position output")
    return prog


def run(prog, inputs, uniforms):
    """Execute prog on one vertex.

    inputs:   {attribute index: [x, y, z, w]}   (missing attributes read 0,0,0,1)
    uniforms: {c index: [x, y, z, w]}
    Returns {output register: [x, y, z, w]} with None for unwritten components.
    This is the documented PICA behaviour for the instructions above and
    decodes the words independently of assemble().
    """
    import math
    v = {i: list(inputs.get(i, [0.0, 0.0, 0.0, 1.0])) for i in range(16)}
    r = {i: [0.0] * 4 for i in range(16)}
    o = {i: [None] * 4 for i in range(16)}

    def src(num, field):
        vec = v[num] if num < 0x10 else r[num - 0x10] if num < 0x20 else uniforms[num - 0x20]
        sw = field >> 1
        out = [vec[(sw >> s) & 3] for s in (6, 4, 2, 0)]
        return [-x for x in out] if field & 1 else out

    pc = prog.entry
    for _ in range(4096):
        word = prog.code[pc]
        pc += 1
        op = word >> 26
        if op == 0x22:
            return o
        if op == 0x21:
            continue
        desc = prog.opdesc[word & 0x7F]
        d = (word >> 21) & 0x1F
        a = src((word >> 12) & 0x7F, (desc >> 4) & 0x1FF)
        b = src((word >> 7) & 0x1F, (desc >> 13) & 0x1FF)
        if op == 0x00:
            res = [x + y for x, y in zip(a, b)]
        elif op == 0x01:
            res = [sum(x * y for x, y in zip(a[:3], b[:3]))] * 4
        elif op == 0x02:
            res = [sum(x * y for x, y in zip(a, b))] * 4
        elif op == 0x08:
            res = [x * y for x, y in zip(a, b)]
        elif op == 0x09:
            res = [1.0 if x >= y else 0.0 for x, y in zip(a, b)]
        elif op == 0x0A:
            res = [1.0 if x < y else 0.0 for x, y in zip(a, b)]
        elif op == 0x0C:
            res = [max(x, y) for x, y in zip(a, b)]
        elif op == 0x0D:
            res = [min(x, y) for x, y in zip(a, b)]
        elif op == 0x13:
            res = a
        elif op == 0x0B:
            res = [float(math.floor(x)) for x in a]
        elif op == 0x0E:
            res = [1.0 / a[0]] * 4
        elif op == 0x0F:
            res = [1.0 / math.sqrt(a[0])] * 4
        else:
            raise AsmError("run: unsupported opcode %#x" % op)
        target = o[d] if d < 0x10 else r[d - 0x10]
        for i in range(4):
            if desc & (8 >> i):
                target[i] = res[i]
    raise AsmError("run: no 'end' reached")


def header(prog, name, source):
    total, mask, maps, mode, clock = prog.outmap

    def words(vals):
        return "".join("    " + ", ".join("0x%08X" % w for w in vals[i:i + 6]) + ",\n"
                       for i in range(0, len(vals), 6))

    return (
        "/* Generated by picasm.py from %s - do not edit. */\n"
        "static const uint32_t %s_code[%d] = {\n%s};\n"
        "static const uint32_t %s_opdesc[%d] = {\n%s};\n"
        "#define %s_ENTRY        %d\n"
        "#define %s_OUTMAP_TOTAL %d\n"
        "#define %s_OUTMAP_MASK  0x%X\n"
        "#define %s_OUTMAP       { %s }\n"
        "#define %s_OUTATTR_MODE  %d\n"
        "#define %s_OUTATTR_CLOCK 0x%X\n"
        % (source, name, len(prog.code), words(prog.code), name, len(prog.opdesc), words(prog.opdesc),
           name.upper(), prog.entry, name.upper(), total, name.upper(), mask,
           name.upper(), ", ".join("0x%08X" % w for w in maps),
           name.upper(), mode, name.upper(), clock))


def main():
    ap = argparse.ArgumentParser(description="assemble a PICA200 vertex shader into a C header")
    ap.add_argument("source")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--name", default="pica_ff", help="C identifier prefix")
    a = ap.parse_args()
    try:
        prog = assemble(open(a.source).read())
    except AsmError as e:
        sys.exit("%s: %s" % (a.source, e))
    import os
    with open(a.output, "w") as f:
        f.write(header(prog, a.name, os.path.basename(a.source)))


if __name__ == "__main__":
    main()
