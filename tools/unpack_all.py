#!/usr/bin/env python3
"""unpack_all.py SRC_DIR DST_DIR - unpack every SQZ/TRK file of the game (SQZ or DIET)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unsqz import unsqz
from undiet import undiet, is_diet

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
ok = True
for name in sorted(os.listdir(src)):
    if not name.upper().endswith((".SQZ", ".TRK")):
        continue
    d = open(os.path.join(src, name), "rb").read()
    try:
        if is_diet(d):
            r, how = undiet(d), "diet"
        elif name.upper() == "PRE2.SQZ":
            r, how = unsqz(d, clear=0x101, end=0x100), "sqz-cdrun"
            name = "PRE2.EXE"
        else:
            r, how = unsqz(d), f"sqz-{d[1]:02x}"
    except Exception as e:
        print(f"{name}: FAILED {e}"); ok = False; continue
    open(os.path.join(dst, name), "wb").write(r)
    print(f"{name:14s} {how:10s} {len(d):7d} -> {len(r):7d}")
sys.exit(0 if ok else 1)
