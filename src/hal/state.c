/* state.c - save states: the whole machine (CPU, memory, VGA, sound, PIC/PIT/keyboard, open files)
 * in one block of bytes.
 *
 * Layout: "P2ST", u32 version, u32 program hash, then the modules in a fixed order.  The hash is
 * that of the loaded PRE2.EXE image, so a state is only accepted by the game version that made it.
 * A state is taken between video frames; loading one replaces the machine at such a boundary. */
#include "hal.h"
#include "state.h"
#include "../qol/qol.h"
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC 0x54533250u      /* "P2ST" */
#define STATE_VERSION 1u

void st_io(StateIO *s, void *p, size_t n)
{
    if (s->err) return;
    if (s->write) {
        if (s->pos + n > s->cap) {
            size_t cap = s->cap ? s->cap : 1u << 20;
            uint8_t *nb;
            if (s->fixed) { s->err = 1; return; }
            while (cap < s->pos + n) cap *= 2u;
            nb = (uint8_t *)realloc(s->buf, cap);
            if (nb == NULL) { s->err = 1; return; }
            s->buf = nb;
            s->cap = cap;
        }
        memcpy(s->buf + s->pos, p, n);
    } else {
        if (s->pos + n > s->cap) { s->err = 1; return; }
        memcpy(p, s->buf + s->pos, n);
    }
    s->pos += n;
}

static void all_state(StateIO *s)
{
    cpu_state(s);
    vga_state(s);
    sb_state(s);
    machine_state(s);
    dos_state(s);
    qol_state(s);
}

int hal_state_save(uint8_t **buf, size_t *len)
{
    StateIO s;
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, hash = hal_program_hash();
    memset(&s, 0, sizeof s);
    s.write = 1;
    ST(&s, magic);
    ST(&s, version);
    ST(&s, hash);
    all_state(&s);
    if (s.err) { free(s.buf); return 0; }
    *buf = s.buf;
    *len = s.pos;
    return 1;
}

int hal_state_load(const uint8_t *buf, size_t len)
{
    StateIO s;
    uint32_t magic = 0, version = 0, hash = 0;
    uint8_t *back = NULL;
    size_t back_len = 0;
    memset(&s, 0, sizeof s);
    s.buf = (uint8_t *)buf;          /* read only: st_io never writes to it in this mode */
    s.cap = len;
    s.fixed = 1;
    ST(&s, magic);
    ST(&s, version);
    ST(&s, hash);
    if (s.err || magic != STATE_MAGIC || version != STATE_VERSION || hash != hal_program_hash()) return 0;
    /* a damaged file must not leave half a machine: keep the present one to fall back on */
    if (!hal_state_save(&back, &back_len)) return 0;
    all_state(&s);
    if (s.err || s.pos != len) {
        StateIO r;
        memset(&r, 0, sizeof r);
        r.buf = back;
        r.cap = back_len;
        r.fixed = 1;
        r.pos = 12;
        all_state(&r);
        free(back);
        hal_state_loaded();
        return 0;
    }
    free(back);
    hal_state_loaded();
    return 1;
}
