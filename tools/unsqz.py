#!/usr/bin/env python3
"""unsqz.py - unpack Titus SQZ files (LZW 0x10 / Huffman+RLE 0x00).

Usage: unsqz.py IN [OUT]
Written from the format description on the ModdingWiki (Titus Interactive SQZ Compression).
"""
import sys


def unsqz(data: bytes, clear=0x100, end=0x101) -> bytes:
    size = ((data[0] & 0x0F) << 16) | data[2] | (data[3] << 8)
    kind = data[1]
    if kind == 0x10:
        return _lzw(data, 4, size, clear, end)
    if kind == 0x00:
        return _huff(data, size)
    raise ValueError(f"unknown SQZ type {kind:#x}")


def _bits(data, pos):
    for b in data[pos:]:
        for i in range(7, -1, -1):
            yield (b >> i) & 1


def _lzw(data, pos, size, clear, end):
    out = bytearray()
    bits = _bits(data, pos)
    nbit = 9
    dic = [bytes([i]) for i in range(256)] + [b"", b""]
    prev = None
    while len(out) < size:
        code = 0
        try:
            for _ in range(nbit):
                code = (code << 1) | next(bits)
        except StopIteration:
            break
        if code == clear:
            nbit = 9
            del dic[258:]
            prev = None
            continue
        if code == end:
            break
        if code < len(dic):
            cur = dic[code]
            if prev is not None and len(dic) < 4096:
                dic.append(prev + cur[:1])
        else:
            cur = prev + prev[:1]
            dic.append(cur)
        out += cur
        prev = cur
        if len(dic) == (1 << nbit) and nbit < 12:
            nbit += 1
    return bytes(out[:size])


def _huff(data, size):
    tsize = data[4] | (data[5] << 8)
    tree = [data[6 + 2 * i] | (data[7 + 2 * i] << 8) for i in range(tsize // 2)]
    out = bytearray()
    state, last, count = 0, 0, 0
    node = 0
    for bit in _bits(data, 6 + tsize):
        v = tree[node + bit]
        if not (v & 0x8000):
            node = v // 2
            continue
        node = 0
        cw = v & 0x7FFF
        lo, hi = cw & 0xFF, cw >> 8
        if state == 0:
            if hi == 0:
                last = lo
                out.append(lo)
            elif lo == 0:
                state = 1
            elif lo == 1:
                state = 2
            else:
                out += bytes([last]) * lo
        elif state == 1:
            out += bytes([last]) * cw
            state = 0
        elif state == 2:
            count = lo << 8
            state = 3
        else:
            out += bytes([last]) * (count + lo)
            state = 0
        if len(out) >= size:
            break
    return bytes(out[:size])


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    r = unsqz(d)
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(r)
    print(f"{sys.argv[1]}: type {d[1]:#04x} -> {len(r)} bytes")
