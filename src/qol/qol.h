/* qol.h - additions to the original game that run beside it (the program itself is not changed). */
#ifndef P2_QOL_H
#define P2_QOL_H

#include "../core/cpu.h"
#include "../hal/hal.h"
#include "../hal/state.h"

void qol_init(void);                          /* the program was loaded (hal_init) */
void qol_reset(void);                         /* forget what was drawn for the present picture */
void qol_state(StateIO *s);                   /* part of a saved state (state.c) */
void qol_sandbox_trap(Cpu *c, uint8_t n);     /* a trap while g_hal_sandbox is set */

/* widescreen (wide.c): the playfield's picture is `width` pixels wide instead of 320 */
#define WIDE_OFF 320
#define WIDE_16_10 384
#define WIDE_16_9 428
#define WIDE_21_9 560
#define WIDE_MAX 560
int qol_wide_supported(void);                 /* the loaded program is the version wide.c knows */
void qol_set_wide(int width);
int qol_wide_width(void);
void qol_wide_frame(HostFrame *f);            /* widens the frame that vga_render made, when it can */

#endif
