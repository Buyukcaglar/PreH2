#!/usr/bin/env python3
"""sheet.py OUT.png FILE... - a contact sheet (6 columns, 320x240 cells) of PNG frames."""
import sys, zlib, struct
def rd(p):
    d = open(p, 'rb').read(); i = 8; w = h = 0; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]; t = d[i+4:i+8]; c = d[i+8:i+8+n]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8])
        if t == b'IDAT': idat += c
        i += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y*(w*3+1)+1:(y+1)*(w*3+1)] for y in range(h)]
files = sys.argv[2:]
tw, th, cols = 320, 240, 5
rn = (len(files) + cols - 1) // cols
W, H = tw*cols, th*rn
out = [bytearray(W*3) for _ in range(H)]
for k, f in enumerate(files):
    w, h, rows = rd(f); ox = (k % cols)*tw; oy = (k//cols)*th
    for y in range(th):
        r = rows[y*h//th]
        for x in range(tw):
            sx = x*w//tw
            out[oy+y][(ox+x)*3:(ox+x)*3+3] = r[sx*3:sx*3+3]
raw = b''.join(b'\0' + bytes(r) for r in out)
def ch(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
