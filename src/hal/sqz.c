/* sqz.c - Titus SQZ unpacker: LZW (type 10h) and Huffman + RLE (type 00h).
 * Written from the format description on the ModdingWiki ("Titus Interactive SQZ Compression").
 * PRE2.SQZ, which CDRUN.COM starts, uses LZW with the CLEAR and END codes swapped (cdrun = 1). */
#include "hal.h"
#include <stdlib.h>
#include <string.h>

typedef struct Bits {
    const uint8_t *p;
    size_t n, pos;      /* pos in bits */
} Bits;

static int getbits(Bits *b, int k, unsigned *v)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < k; i++) {
        size_t byte = b->pos >> 3;
        if (byte >= b->n) return 0;
        r = (r << 1) | ((b->p[byte] >> (7u - (b->pos & 7u))) & 1u);
        b->pos++;
    }
    *v = r;
    return 1;
}

static int lzw(const uint8_t *in, size_t n, uint8_t *out, size_t size, unsigned clear, unsigned end)
{
    /* dictionary: prefix code + last byte + length */
    static uint16_t pre[4096];
    static uint8_t suf[4096], first[4096];
    static uint16_t len[4096];
    Bits b = {in + 4, n - 4, 0};
    unsigned nbit = 9, dict = 258, code, prev = 0xFFFFu;
    size_t o = 0;
    unsigned i;
    for (i = 0; i < 256; i++) { pre[i] = 0xFFFFu; suf[i] = (uint8_t)i; first[i] = (uint8_t)i; len[i] = 1; }
    while (o < size) {
        unsigned c, l, k;
        uint8_t fb;
        if (!getbits(&b, (int)nbit, &code)) break;
        if (code == clear) { nbit = 9; dict = 258; prev = 0xFFFFu; continue; }
        if (code == end) break;
        if (code < dict) {
            fb = first[code];
            if (prev != 0xFFFFu && dict < 4096u) {
                pre[dict] = (uint16_t)prev; suf[dict] = fb; first[dict] = first[prev];
                len[dict] = (uint16_t)(len[prev] + 1u); dict++;
            }
            c = code;
        } else {
            if (prev == 0xFFFFu || code != dict || dict >= 4096u) return 0;
            fb = first[prev];
            pre[dict] = (uint16_t)prev; suf[dict] = fb; first[dict] = first[prev];
            len[dict] = (uint16_t)(len[prev] + 1u); dict++;
            c = code;
        }
        l = len[c];
        if (o + l > size) l = (unsigned)(size - o);
        /* write the string backwards */
        {
            unsigned full = len[c], x = c;
            for (k = full; k > 0; k--) {
                if (k - 1u < l) out[o + k - 1u] = suf[x];
                x = pre[x];
            }
        }
        o += l;
        prev = c;
        if (dict == (1u << nbit) && nbit < 12u) nbit++;
    }
    return o == size;
}

static int huff(const uint8_t *in, size_t n, uint8_t *out, size_t size)
{
    size_t tsize, o = 0;
    Bits b;
    unsigned node = 0, state = 0, last = 0, count = 0;
    if (n < 6) return 0;
    tsize = (size_t)(in[4] | (in[5] << 8));
    if (6u + tsize > n) return 0;
    b.p = in + 6 + tsize;
    b.n = n - 6 - tsize;
    b.pos = 0;
    while (o < size) {
        unsigned bit, v, lo, hi;
        if (!getbits(&b, 1, &bit)) break;
        if ((node + bit) * 2u + 1u >= tsize) return 0;
        v = (unsigned)(in[6 + (node + bit) * 2] | (in[7 + (node + bit) * 2] << 8));
        if (!(v & 0x8000u)) { node = v / 2u; continue; }
        node = 0;
        v &= 0x7FFFu;
        lo = v & 0xFFu;
        hi = v >> 8;
        switch (state) {
        case 0:
            if (hi == 0) { last = lo; out[o++] = (uint8_t)lo; }
            else if (lo == 0) state = 1;
            else if (lo == 1) state = 2;
            else { while (lo-- && o < size) out[o++] = (uint8_t)last; }
            break;
        case 1:
            while (v-- && o < size) out[o++] = (uint8_t)last;
            state = 0;
            break;
        case 2: count = lo << 8; state = 3; break;
        default:
            count += lo;
            while (count-- && o < size) out[o++] = (uint8_t)last;
            state = 0;
            break;
        }
    }
    return o == size;
}

int sqz_unpack(const uint8_t *in, size_t n, uint8_t **out, size_t *outlen, int cdrun)
{
    size_t size;
    int ok;
    *out = NULL;
    if (n < 4) return 0;
    size = ((size_t)(in[0] & 0x0Fu) << 16) | in[2] | ((size_t)in[3] << 8);
    *out = (uint8_t *)malloc(size ? size : 1u);
    if (*out == NULL) return 0;
    if (in[1] == 0x10) ok = lzw(in, n, *out, size, cdrun ? 0x101u : 0x100u, cdrun ? 0x100u : 0x101u);
    else if (in[1] == 0x00) ok = huff(in, n, *out, size);
    else ok = 0;
    *outlen = size;
    return ok;
}
