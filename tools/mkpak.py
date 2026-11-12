"""mkpak.py GAMEDIR OUT.pak - packs the game's files into one file for src/hal/pack.c.

Used by `build.cmd private` for a personal build that carries the game inside PreH2.exe.
Such a build contains the game's data: it must not be published or shared."""
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    names = sorted(n for n in os.listdir(src)
                   if os.path.isfile(os.path.join(src, n)) and n.upper().endswith((".SQZ", ".TRK")))
    if "PRE2.SQZ" not in [n.upper() for n in names]:
        sys.exit(f"PRE2.SQZ not found in {src}")
    blobs = [open(os.path.join(src, n), "rb").read() for n in names]
    head = bytearray(b"P2PK" + struct.pack("<I", len(names)))
    off = 8 + 24 * len(names)
    for n, b in zip(names, blobs):
        nm = n.upper().encode("ascii")
        if len(nm) > 15:
            sys.exit(f"name too long: {n}")
        head += nm.ljust(16, b"\0") + struct.pack("<II", off, len(b))
        off += len(b)
    with open(out, "wb") as f:
        f.write(head)
        for b in blobs:
            f.write(b)
    print(f"{out}: {len(names)} files, {off} bytes")


if __name__ == "__main__":
    main()
