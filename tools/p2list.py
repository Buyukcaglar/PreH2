#!/usr/bin/env python3
"""p2list.py START [END] - linear disassembly of code segment 0020 (hex offsets)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p2dis
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
code = p2dis.codeseg(p2dis.load())
a = int(sys.argv[1], 16); b = int(sys.argv[2], 16) if len(sys.argv) > 2 else a + 0x80
md = Cs(CS_ARCH_X86, CS_MODE_16)
for i in md.disasm(code[a:b], a):
    print(f"{i.address:04x}  {i.bytes.hex():14s} {i.mnemonic} {i.op_str}".replace("0xffff", "0x"))
