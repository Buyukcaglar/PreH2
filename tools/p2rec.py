#!/usr/bin/env python3
"""p2rec.py - the static recompiler: PRE2.EXE's code segment 0020 -> src/gen/gen_all.c.

Every basic block of the code (found by p2dis.explore from the entry point, the interrupt handlers,
tools/p2hints.py and the executed offsets in work/cov.txt) becomes one C function over the shared
machine state (src/core/cpu.h).  The instructions' semantics are the interpreter's helpers
(cpu_alu, cpu_shift, ...), so the recompiled code and the interpreter agree flag for flag.  A block
ends at every control transfer and after the instructions after which an interrupt may be taken
(STI, POPF, OUT, IN, INT, HLT), so interrupts are delivered between blocks as the interpreter does.

The constants that the MZ relocations patch (segment values) are written as REL(x): the load
segment is added at run time, as the DOS loader does.

Usage: python tools/p2rec.py            (writes src/gen/gen_all.c)
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p2dis
import p2hints
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

ROOT = p2dis.ROOT
R16 = {"ax": "AX", "cx": "CX", "dx": "DX", "bx": "BX", "sp": "SP", "bp": "BP", "si": "SI", "di": "DI"}
R8 = {"al": 0, "cl": 1, "dl": 2, "bl": 3, "ah": 4, "ch": 5, "dh": 6, "bh": 7}
SR = {"es": "ES", "cs": "CS", "ss": "SS", "ds": "DS"}
ALU = {"add": 0, "or": 1, "adc": 2, "sbb": 3, "and": 4, "sub": 5, "xor": 6, "cmp": 7}
SHIFT = {"rol": 0, "ror": 1, "rcl": 2, "rcr": 3, "shl": 4, "sal": 4, "shr": 5, "sar": 7}
JCC = {"jo": 0, "jno": 1, "jb": 2, "jae": 3, "je": 4, "jne": 5, "jbe": 6, "ja": 7,
       "js": 8, "jns": 9, "jp": 10, "jnp": 11, "jl": 12, "jge": 13, "jle": 14, "jg": 15}
SEGPFX = {0x26: "ES", 0x2E: "CS", 0x36: "SS", 0x3E: "DS"}
# instructions after which the block ends although control continues (interrupt windows, I/O)
BREAK_AFTER = {"sti", "popf", "out", "in", "hlt", "int", "iret", "cli"}


class Unsupported(Exception):
    pass


def relocs_in_code(exe):
    crlc = struct.unpack_from("<H", exe, 6)[0]
    lfarlc = struct.unpack_from("<H", exe, 0x18)[0]
    sites = set()
    for i in range(crlc):
        off, seg = struct.unpack_from("<HH", exe, lfarlc + 4 * i)
        lin = seg * 16 + off - p2dis.CODESEG * 16
        if 0 <= lin < (p2dis.DATASEG - p2dis.CODESEG) * 16:
            sites.add(lin)
    return sites


class Gen:
    def __init__(self, code, relocs):
        self.code = code
        self.relocs = relocs
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True

    def dec(self, a):
        return next(self.md.disasm(self.code[a:a + 16], a))

    # ---- operands ----
    def imm(self, i, op, w):
        v = op.imm & (0xFFFF if w else 0xFF)
        if w and i.imm_size == 2 and (i.address + i.imm_offset) in self.relocs:
            return f"REL(0x{v:04X}u)"
        return f"0x{v:X}u"

    def memaddr(self, i, op, segpfx):
        m = op.mem
        parts = []
        base = i.reg_name(m.base) if m.base else None
        index = i.reg_name(m.index) if m.index else None
        if base:
            parts.append(f"c->r[{R16[base]}]")
        if index:
            parts.append(f"c->r[{R16[index]}]")
        disp = m.disp & 0xFFFF
        if disp or not parts:
            if i.disp_size == 2 and (i.address + i.disp_offset) in self.relocs:
                parts.append(f"REL(0x{disp:04X}u)")
            else:
                parts.append(f"0x{disp:X}u")
        off = parts[0] if len(parts) == 1 and not parts[0].startswith("c->") else f"(uint16_t)({' + '.join(parts)})"
        if segpfx:
            seg = segpfx
        elif m.segment:
            seg = SR[i.reg_name(m.segment)]
        elif base == "bp":
            seg = "SS"
        else:
            seg = "DS"
        return f"c->s[{seg}]", off

    def width(self, i, op):
        if op.type == X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in R8:
                return 0
            return 1
        return 1 if op.size == 2 else 0

    def get(self, i, op, w, segpfx):
        if op.type == X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in R16:
                return f"c->r[{R16[n]}]"
            if n in R8:
                return f"r8get(c, {R8[n]})"
            if n in SR:
                return f"c->s[{SR[n]}]"
            raise Unsupported(n)
        if op.type == X86_OP_IMM:
            return self.imm(i, op, w)
        seg, off = self.memaddr(i, op, segpfx)
        return f"rws({seg}, {off})" if w else f"rbs({seg}, {off})"

    def set(self, i, op, w, val, segpfx):
        if op.type == X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in R16:
                return f"c->r[{R16[n]}] = (uint16_t)({val});"
            if n in R8:
                return f"r8set(c, {R8[n]}, (uint8_t)({val}));"
            if n in SR:
                return f"c->s[{SR[n]}] = (uint16_t)({val});"
            raise Unsupported(n)
        seg, off = self.memaddr(i, op, segpfx)
        return f"wws({seg}, {off}, (uint16_t)({val}));" if w else f"wbs({seg}, {off}, (uint8_t)({val}));"

    # ---- one instruction: (C lines, ends_block) ----
    def emit(self, i):
        raw = bytes(i.bytes)
        k = 0
        segpfx = None
        rep = 0
        while raw[k] in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3, 0xF0):
            if raw[k] in SEGPFX:
                segpfx = SEGPFX[raw[k]]
            elif raw[k] in (0xF2, 0xF3):
                rep = raw[k]
            k += 1
        opc = raw[k]
        mn = i.mnemonic
        ops = i.operands
        nxt = (i.address + i.size) & 0xFFFF
        a = i.address
        L = []
        if mn in ALU:
            w = self.width(i, ops[0])
            L.append(f"{{ uint32_t r = cpu_alu(c, {ALU[mn]}, {self.get(i, ops[0], w, segpfx)}, {self.get(i, ops[1], w, segpfx)}, {w});")
            if mn != "cmp":
                L.append("  " + self.set(i, ops[0], w, "r", segpfx))
            else:
                L.append("  (void)r;")
            L.append("}")
            return L, False
        if mn == "test":
            w = self.width(i, ops[0])
            return [f"(void)cpu_alu(c, 4, {self.get(i, ops[0], w, segpfx)}, {self.get(i, ops[1], w, segpfx)}, {w});"], False
        if mn in ("inc", "dec"):
            w = self.width(i, ops[0])
            return [self.set(i, ops[0], w, f"cpu_incdec(c, {self.get(i, ops[0], w, segpfx)}, {1 if mn == 'dec' else 0}, {w})", segpfx)], False
        if mn == "neg":
            w = self.width(i, ops[0])
            return [f"{{ uint32_t v = {self.get(i, ops[0], w, segpfx)}; uint32_t r = cpu_alu(c, 5, 0, v, {w});",
                    "  " + self.set(i, ops[0], w, "r", segpfx),
                    "  if (v) c->flags |= F_CF; else c->flags &= (uint16_t)~F_CF; }"], False
        if mn == "not":
            w = self.width(i, ops[0])
            return [self.set(i, ops[0], w, f"~{self.get(i, ops[0], w, segpfx)}", segpfx)], False
        if mn == "mov":
            w = self.width(i, ops[0]) if ops[0].type == X86_OP_REG else self.width(i, ops[1]) if ops[1].type == X86_OP_REG else (1 if ops[0].size == 2 else 0)
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in SR or ops[1].type == X86_OP_REG and i.reg_name(ops[1].reg) in SR:
                w = 1
            return [self.set(i, ops[0], w, self.get(i, ops[1], w, segpfx), segpfx)], False
        if mn == "xchg":
            w = self.width(i, ops[0])
            return [f"{{ uint32_t t0 = {self.get(i, ops[0], w, segpfx)}; uint32_t t1 = {self.get(i, ops[1], w, segpfx)};",
                    "  " + self.set(i, ops[0], w, "t1", segpfx), "  " + self.set(i, ops[1], w, "t0", segpfx), "}"], False
        if mn == "lea":
            seg, off = self.memaddr(i, ops[1], segpfx)
            return [f"c->r[{R16[i.reg_name(ops[0].reg)]}] = (uint16_t)({off});"], False
        if mn in ("les", "lds"):
            seg, off = self.memaddr(i, ops[1], segpfx)
            sreg = "ES" if mn == "les" else "DS"
            return [f"{{ uint16_t o = {off}; uint16_t s = {seg}; c->r[{R16[i.reg_name(ops[0].reg)]}] = rws(s, o); c->s[{sreg}] = rws(s, (uint16_t)(o + 2u)); }}"], False
        if mn == "push":
            if ops[0].type == X86_OP_IMM:
                return [f"push16(c, {self.imm(i, ops[0], 1)});"], False
            return [f"push16(c, {self.get(i, ops[0], 1, segpfx)});"], False
        if mn == "pop":
            return [f"{{ uint16_t v = pop16(c); {self.set(i, ops[0], 1, 'v', segpfx)} }}"], False
        if mn in SHIFT:
            w = self.width(i, ops[0])
            if len(ops) == 1:
                cnt = "1u"
            elif ops[1].type == X86_OP_IMM:
                cnt = f"{ops[1].imm & 0xFF}u"
            else:
                cnt = "(unsigned)(c->r[CX] & 0xFFu)"
            return [self.set(i, ops[0], w, f"cpu_shift(c, {SHIFT[mn]}, {self.get(i, ops[0], w, segpfx)}, {cnt}, {w})", segpfx)], False
        if mn in ("mul", "imul", "div", "idiv") and len(ops) == 1:
            w = self.width(i, ops[0])
            code = {"mul": 4, "imul": 5, "div": 6, "idiv": 7}[mn]
            if mn in ("div", "idiv"):
                return [f"c->ip = 0x{nxt:04X}u; c->op_ip = 0x{a:04X}u; c->op_cs = c->s[CS];",
                        f"cpu_mul(c, {code}, {self.get(i, ops[0], w, segpfx)}, {w});",
                        f"if (c->ip != 0x{nxt:04X}u) return 1;"], False
            return [f"cpu_mul(c, {code}, {self.get(i, ops[0], w, segpfx)}, {w});"], False
        if mn == "imul":
            src = self.get(i, ops[1], 1, segpfx)
            mul = self.imm(i, ops[2], 1) if len(ops) == 3 else self.get(i, ops[0], 1, segpfx)
            return [f"{{ int32_t r = (int32_t)(int16_t){src} * (int32_t)(int16_t){mul};",
                    f"  c->r[{R16[i.reg_name(ops[0].reg)]}] = (uint16_t)r;",
                    "  if (r != (int16_t)r) c->flags |= F_CF | F_OF; else c->flags &= (uint16_t)~(F_CF | F_OF); }"], False
        if opc == 0x98:
            return ["c->r[AX] = (uint16_t)(int16_t)(int8_t)c->r[AX];"], False
        if opc == 0x99:
            return ["c->r[DX] = (c->r[AX] & 0x8000u) ? 0xFFFFu : 0u;"], False
        if opc == 0xD7:
            seg = segpfx or "DS"
            return [f"r8set(c, 0, rbs(c->s[{seg}], (uint16_t)(c->r[BX] + (c->r[AX] & 0xFFu))));"], False
        if opc in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0x6C, 0x6D, 0x6E, 0x6F):
            seg = {"ES": 0, "CS": 1, "SS": 2, "DS": 3}[segpfx] if segpfx else -1
            return [f"cpu_string(c, 0x{opc:02X}, {seg}, 0x{rep:02X});"], False
        if mn in ("clc", "stc", "cli", "sti", "cld", "std", "cmc"):
            return [{"clc": "c->flags &= (uint16_t)~F_CF;", "stc": "c->flags |= F_CF;",
                     "cli": "c->flags &= (uint16_t)~F_IF;", "sti": "c->flags |= F_IF; c->irq_pending = 1;",
                     "cld": "c->flags &= (uint16_t)~F_DF;", "std": "c->flags |= F_DF;",
                     "cmc": "c->flags ^= F_CF;"}[mn]], False
        if mn == "pushf":
            return ["push16(c, (uint16_t)(c->flags | 0xF002u));"], False
        if mn == "popf":
            return ["c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u); c->irq_pending = 1;"], False
        if mn == "lahf":
            return ["r8set(c, 4, (uint8_t)(c->flags | 2u));"], False
        if mn == "sahf":
            return ["c->flags = (uint16_t)((c->flags & 0xFF00u) | ((c->r[AX] >> 8) & 0xD5u) | 2u);"], False
        if mn == "nop":
            return [], False
        if mn == "in":
            port = f"0x{ops[1].imm & 0xFF:X}u" if ops[1].type == X86_OP_IMM else "c->r[DX]"
            if i.reg_name(ops[0].reg) == "al":
                return [f"r8set(c, 0, io_in8({port}));"], False
            return [f"c->r[AX] = (uint16_t)(io_in8({port}) | (io_in8((uint16_t)({port} + 1u)) << 8));"], False
        if mn == "out":
            port = f"0x{ops[0].imm & 0xFF:X}u" if ops[0].type == X86_OP_IMM else "c->r[DX]"
            if i.reg_name(ops[1].reg) == "al":
                return [f"io_out8({port}, (uint8_t)c->r[AX]);"], False
            return [f"io_out8({port}, (uint8_t)c->r[AX]); io_out8((uint16_t)({port} + 1u), (uint8_t)(c->r[AX] >> 8));"], False
        # ---- control transfers ----
        if mn in JCC:
            t = ops[0].imm & 0xFFFF
            return [f"c->ip = cpu_cond(c, {JCC[mn]}) ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn in ("loop", "loope", "loopne"):
            t = ops[0].imm & 0xFFFF
            cond = {"loop": "1", "loope": "(c->flags & F_ZF)", "loopne": "!(c->flags & F_ZF)"}[mn]
            return [f"c->r[CX]--; c->ip = (c->r[CX] != 0 && {cond}) ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn == "jcxz":
            t = ops[0].imm & 0xFFFF
            return [f"c->ip = c->r[CX] == 0 ? 0x{t:04X}u : 0x{nxt:04X}u;"], True
        if mn == "jmp":
            if ops[0].type == X86_OP_IMM and len(ops) == 1:
                return [f"c->ip = 0x{ops[0].imm & 0xFFFF:04X}u;"], True
            return [f"c->ip = {self.get(i, ops[0], 1, segpfx)};"], True
        if mn == "call":
            if ops[0].type == X86_OP_IMM:
                return [f"push16(c, 0x{nxt:04X}u); c->ip = 0x{ops[0].imm & 0xFFFF:04X}u;"], True
            return [f"{{ uint16_t t = {self.get(i, ops[0], 1, segpfx)}; push16(c, 0x{nxt:04X}u); c->ip = t; }}"], True
        if mn in ("lcall", "ljmp"):
            if len(ops) == 2:
                segv = ops[0].imm & 0xFFFF
                segs = f"REL(0x{segv:04X}u)" if (a + k + 3) in self.relocs else f"0x{segv:04X}u"
                pre = f"push16(c, c->s[CS]); push16(c, 0x{nxt:04X}u); " if mn == "lcall" else ""
                return [f"{pre}c->s[CS] = {segs}; c->ip = 0x{ops[1].imm & 0xFFFF:04X}u;"], True
            seg, off = self.memaddr(i, ops[0], segpfx)
            pre = "push16(c, c->s[CS]); push16(c, 0x%04Xu); " % nxt if mn == "lcall" else ""
            return [f"{{ uint16_t s = {seg}; uint16_t o = {off}; uint16_t nip = rws(s, o), ncs = rws(s, (uint16_t)(o + 2u));",
                    f"  {pre}c->s[CS] = ncs; c->ip = nip; }}"], True
        if mn == "ret":
            if ops:
                return [f"c->ip = pop16(c); c->r[SP] = (uint16_t)(c->r[SP] + {ops[0].imm & 0xFFFF}u);"], True
            return ["c->ip = pop16(c);"], True
        if mn == "retf":
            n = ops[0].imm & 0xFFFF if ops else 0
            return [f"c->ip = pop16(c); c->s[CS] = pop16(c); c->r[SP] = (uint16_t)(c->r[SP] + {n}u);"], True
        if mn == "iret":
            return ["c->ip = pop16(c); c->s[CS] = pop16(c); c->flags = (uint16_t)((pop16(c) & 0x0FD5u) | 0x0002u); c->irq_pending = 1;"], True
        if mn == "int":
            return [f"c->ip = 0x{nxt:04X}u; cpu_interrupt(c, 0x{ops[0].imm & 0xFF:02X});"], True
        if mn == "hlt":
            return [f"c->ip = 0x{nxt:04X}u; c->halted = 1;"], True
        raise Unsupported(f"{mn} {i.op_str} at {a:04x}")


def main():
    exe = open(os.path.join(ROOT, "work", "unpacked", "PRE2.EXE"), "rb").read()
    img = p2dis.load()
    code = p2dis.codeseg(img)
    relocs = relocs_in_code(exe)
    entries = [0x0008] + list(p2hints.ENTRIES)
    covp = os.path.join(ROOT, "work", "cov.txt")
    cov = set()
    if os.path.exists(covp):
        cov = {int(x, 16) for x in open(covp).read().split()}
    # the executed offsets that the analysis does not reach (jump-table targets, handlers) become
    # entries, one region at a time
    ins, funcs, indirect, csw = p2dis.explore(code, entries)
    extra = []
    for a in sorted(cov):
        if a not in ins:
            extra.append(a)
            ins, funcs, indirect, csw = p2dis.explore(code, entries + extra)
    entries = entries + extra
    g = Gen(code, relocs)
    starts = set(entries)
    for a, i in ins.items():
        if i.target is not None:
            starts.add(i.target)
        if i.kind is not None or i.mn in BREAK_AFTER:
            starts.add(a + i.size)
    starts = sorted(s for s in starts if s in ins)
    smc = set(p2hints.SMC)
    out = []
    out.append("/* gen_all.c - GENERATED by tools/p2rec.py from PRE2.EXE's code segment 0020; do not edit.")
    out.append(" * Each function is one basic block of the original program; see tools/p2rec.py. */")
    out.append('#include "gen.h"')
    out.append("")
    names = []
    unsupported = []
    for s in starts:
        a = s
        body = []
        n = 0
        guard = []
        while True:
            dec = g.dec(a)
            if a in smc:
                guard.append((a, code[a]))
            try:
                lines, ends = g.emit(dec)
            except Unsupported as e:
                unsupported.append(str(e))
                body = None
                break
            body.append(f"    /* {a:04X}: {dec.mnemonic} {dec.op_str} */")
            body.extend("    " + l for l in lines)
            n += 1
            a = (a + dec.size) & 0xFFFF
            if ends:
                break
            if dec.mnemonic in BREAK_AFTER:
                body.append(f"    c->ip = 0x{a:04X}u;")
                break
            if a in starts or a not in ins:
                body.append(f"    c->ip = 0x{a:04X}u;")
                break
        if body is None:
            continue
        names.append(s)
        out.append(f"static int b_{s:04X}(Cpu *c)")
        out.append("{")
        for ga, gb in guard:
            out.append(f"    if (g_mem[GAME_CODE_LIN + 0x{ga:04X}u] != 0x{gb:02X}u) return 0;   /* self-modified */")
        out.append(f"    c->icount += {n}u;")
        out.extend(body)
        out.append("    return 1;")
        out.append("}")
        out.append("")
    out.append("const GenBlock g_gen_blocks[] = {")
    for s in names:
        out.append(f"    {{0x{s:04X}u, b_{s:04X}}},")
    out.append("};")
    out.append(f"const unsigned g_gen_block_count = {len(names)}u;")
    os.makedirs(os.path.join(ROOT, "src", "gen"), exist_ok=True)
    with open(os.path.join(ROOT, "src", "gen", "gen_all.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    covered = sum(i.size for i in ins.values())
    print(f"{len(ins)} instructions ({covered:#x} of {len(code):#x} bytes), {len(names)} blocks, "
          f"{len(unsupported)} unsupported, {len(cov)} executed offsets")
    for u in unsupported[:20]:
        print("  unsupported:", u)


if __name__ == "__main__":
    main()
