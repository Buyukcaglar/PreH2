/* state.h - the machine's state as a stream of bytes: save states (state.c) and the snapshots
 * that the widescreen renderer takes around its extra drawing passes (qol/wide.c).
 * Every module has one function that passes its variables through st_io; the same function
 * writes and reads, so the two cannot disagree about the layout. */
#ifndef P2_STATE_H
#define P2_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct StateIO {
    int write;           /* 1: the fields are appended to buf; 0: they are read back from it */
    uint8_t *buf;
    size_t pos, cap;
    int fixed;           /* the buffer is the caller's: it does not grow */
    int err;             /* a read ran past the end, or the buffer could not grow */
} StateIO;

void st_io(StateIO *s, void *p, size_t n);
#define ST(s, v) st_io((s), &(v), sizeof(v))

/* the modules */
void cpu_state(StateIO *s);         /* registers and the whole memory image */
void vga_state(StateIO *s);
void sb_state(StateIO *s);
void machine_state(StateIO *s);
void dos_state(StateIO *s);

/* a complete save state: 1 on success.  hal_state_save allocates *buf (free it with free). */
int hal_state_save(uint8_t **buf, size_t *len);
int hal_state_load(const uint8_t *buf, size_t len);

#endif
