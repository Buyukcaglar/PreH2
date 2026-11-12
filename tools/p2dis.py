#!/usr/bin/env python3
"""p2dis.py - recursive-descent disassembler for PRE2.EXE's code segment (0020).

Reads work/unpacked/PRE2.EXE (unpacked from PRE2.SQZ by unsqz.py), follows control flow from the
entry point and the extra entry points in tools/p2hints.py, and reports coverage, indirect
transfers and code-segment writes.  Library use: load(), explore().
"""
import sys, struct, os, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_CS

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODESEG = 0x20
DATASEG = 0x9FF


def load(path=os.path.join(ROOT, "work", "unpacked", "PRE2.EXE")):
    d = open(path, "rb").read()
    cblp, cp = struct.unpack_from("<HH", d, 2)
    hdr = struct.unpack_from("<H", d, 8)[0] * 16
    size = (cp - 1) * 512 + (cblp or 512)
    return d[hdr:size]


def codeseg(img):
    return img[CODESEG * 16:DATASEG * 16]


JCC = {"je", "jne", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg", "js", "jns", "jo", "jno",
       "jp", "jnp", "jcxz", "loop", "loope", "loopne"}


class Ins:
    __slots__ = ("addr", "size", "mn", "ops", "raw", "target", "kind")


def explore(code, entries, hints=None):
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    md.detail = True
    ins = {}
    owner = {}
    funcs = set()
    indirect = []
    cswrites = []
    work = [(e, True) for e in entries]
    while work:
        a, isfunc = work.pop()
        if isfunc:
            funcs.add(a)
        while a < len(code) and a not in ins:
            if a in owner:  # lands inside an existing instruction
                print(f"warn: overlap at {a:04x}", file=sys.stderr)
                break
            dec = next(md.disasm(code[a:a + 16], a), None)
            if dec is None:
                print(f"warn: bad opcode at {a:04x}", file=sys.stderr)
                break
            i = Ins()
            i.addr, i.size, i.mn, i.ops, i.raw = a, dec.size, dec.mnemonic, dec.op_str, bytes(dec.bytes)
            i.target, i.kind = None, None
            ins[a] = i
            for k in range(a, a + dec.size):
                owner[k] = a
            for op in dec.operands:
                if op.type == X86_OP_MEM and op.mem.segment == X86_REG_CS and dec.mnemonic not in ("jmp", "call", "lcall", "ljmp"):
                    if dec.mnemonic.startswith(("mov", "add", "sub", "and", "or", "xor", "inc", "dec", "not", "neg", "shl", "shr", "rol", "ror", "pop", "xchg", "adc", "sbb")) and dec.operands[0].type == X86_OP_MEM:
                        cswrites.append((a, dec.op_str))
            nxt = a + dec.size
            mn = dec.mnemonic
            imm = dec.operands[0].imm & 0xFFFF if dec.operands and dec.operands[0].type == X86_OP_IMM else None
            if mn in JCC:
                i.kind, i.target = "jcc", imm
                work.append((imm, False))
            elif mn == "jmp":
                if imm is not None and len(dec.operands) == 1:
                    i.kind, i.target = "jmp", imm
                    work.append((imm, False))
                else:
                    i.kind = "ijmp"
                    indirect.append((a, mn, dec.op_str))
                break
            elif mn == "call":
                if imm is not None:
                    i.kind, i.target = "call", imm
                    work.append((imm, True))
                else:
                    i.kind = "icall"
                    indirect.append((a, mn, dec.op_str))
            elif mn in ("lcall", "ljmp"):
                i.kind = "far"
                indirect.append((a, mn, dec.op_str))
                if mn == "ljmp":
                    break
            elif mn in ("ret", "retf", "iret", "hlt"):
                i.kind = mn
                break
            a = nxt
    return ins, funcs, indirect, cswrites


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import p2hints
    img = load()
    code = codeseg(img)
    ins, funcs, indirect, csw = explore(code, [0x0008] + p2hints.ENTRIES)
    covered = sum(i.size for i in ins.values())
    print(f"code bytes {len(code):#x}, decoded {covered:#x} ({100*covered/len(code):.1f}%), {len(ins)} instructions, {len(funcs)} call targets")
    print(f"indirect transfers: {len(indirect)}")
    for a, mn, op in indirect:
        print(f"  {a:04x} {mn} {op}")
    print(f"cs: writes: {len(csw)}")
    tgt = collections.Counter(op.split(',')[0] for _, op in csw)
    for k, v in sorted(tgt.items()):
        print(f"  {v:3d} {k}")


if __name__ == "__main__":
    main()
