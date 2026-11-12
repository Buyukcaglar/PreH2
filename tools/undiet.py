#!/usr/bin/env python3
"""undiet.py - unpack the DIET-packed data files of Prehistorik 2 (signature 9D 89 'dlz').

Transcribed from the game's own decoder, PRE2.EXE 0020:1414..15D7 (bit reader 16F9/1700).
Header (17 bytes): DOS exit stub B4 4C CD 21, 9D 89, 'dlz', ..., byte 0Eh bits 2..7 and word 0Fh
= unpacked size.  Bits are read LSB first from 16-bit little-endian words.
Usage: undiet.py IN [OUT]
"""
import sys, struct


def is_diet(d: bytes) -> bool:
    return len(d) >= 17 and d[4:9] == b"\x9d\x89dlz"


def undiet(d: bytes) -> bytes:
    if not is_diet(d):
        raise ValueError("not a DIET file")
    size = ((d[0x0E] >> 2) << 16) | struct.unpack_from("<H", d, 0x0F)[0]
    pos = 0x11
    out = bytearray()
    bp = struct.unpack_from("<H", d, pos)[0]; pos += 2
    nb = 16

    def bit():
        nonlocal bp, nb, pos
        b = bp & 1
        bp >>= 1
        nb -= 1
        if nb == 0:
            bp = struct.unpack_from("<H", d, pos)[0] if pos + 1 < len(d) else 0
            pos += 2
            nb = 16
        return b

    def byte():
        nonlocal pos
        v = d[pos]; pos += 1
        return v

    def copy(off, n):
        for _ in range(n):
            out.append(out[len(out) + off])

    while True:
        if bit():
            out.append(byte())
            continue
        b = bit()
        bl = byte()
        bh = 0xFF
        if not b:                                  # short match: 2 bytes
            if bit():
                for _ in range(3):
                    bh = ((bh << 1) | bit()) & 0xFF
                bh = (bh - 1) & 0xFF
            elif bl == 0xFF:                       # end marker (bl == bh == FF)
                break
            off = (bh << 8 | bl) - 0x10000
            copy(off, 2)
            continue
        bh = ((bh << 1) | bit()) & 0xFF            # long match
        if not bit():
            dh = 2
            for _ in range(3):
                if bit():
                    break
                bh = ((bh << 1) | bit()) & 0xFF
                dh = (dh * 2) & 0xFF
            bh = (bh - dh) & 0xFF
        off = (bh << 8 | bl) - 0x10000
        dh = 2
        n = None
        for _ in range(4):
            dh += 1
            if bit():
                n = dh
                break
        if n is None:
            if bit():
                dh += 1
                n = dh + bit()
            elif bit():
                n = byte() + 0x11
            else:
                v = 0
                for _ in range(3):
                    v = (v << 1) | bit()
                n = v + 9
        copy(off, n)
    if len(out) != size:
        print(f"warning: unpacked {len(out)} bytes, header says {size}", file=sys.stderr)
    return bytes(out)


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    r = undiet(d)
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(r)
    print(f"{sys.argv[1]}: {len(d)} -> {len(r)} bytes")
