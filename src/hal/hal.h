/* hal.h - the PC that Prehistorik 2 runs on: DOS/BIOS services, PIC, PIT, keyboard, VGA, Sound
 * Blaster (DSP + 8237 DMA channel 1), PC speaker, the virtual clock and the loader.
 *
 * The machine: 640 KB conventional memory, a VGA, a Sound Blaster 2.0 at 220h IRQ 7 DMA 1, no AdLib,
 * no EMS, no XMS, no joystick.  Time is virtual: the clock is the instruction count, at
 * HAL_IPS instructions per second; the host slows it to real time once per video frame. */
#ifndef P2_HAL_H
#define P2_HAL_H

#include <stdint.h>
#include "../core/cpu.h"

#define PIT_HZ 1193182u
#define HAL_IPS 24000000u            /* the virtual CPU's speed, instructions per second */

/* load segment of the EXE image (the PSP is 10h paragraphs below) */
#define LOAD_SEG 0x0110u
#define PSP_SEG (LOAD_SEG - 0x10u)

/* ---- the host's side (host_*.c) ---- */
typedef struct HostFrame {
    int width, height;                /* logical size of the picture (320x200, 640x480, 640x400 text) */
    const uint32_t *pixels;           /* width*height 0x00RRGGBB */
} HostFrame;

void host_present(const HostFrame *f);          /* a new picture (once per video frame) */
void host_audio(const int16_t *stereo, int frames);   /* 44100 Hz stereo samples, in order */
uint64_t host_time_us(void);
void host_sleep_us(uint64_t us);
void host_pump(void);                           /* window messages; may call hal_key */
int host_quit_requested(void);
const char *host_game_dir(void);                /* the folder with PRE2.SQZ (PREH2) */

/* ---- called by the host ---- */
int hal_init(char *err, int errlen);            /* loads PRE2.SQZ and prepares the machine */
int hal_frame(void);                            /* runs one video frame; 0 when the program ended */
void hal_key(uint8_t scancode, int down, int extended);   /* PC set-1 scancodes */
void hal_fatal(const char *msg);
extern char g_hal_exit_msg[256];                /* the DOS program's last message (int 21h/09h) */
int hal_exited(void);

/* ---- internal ---- */
void vga_reset(void);
uint8_t vga_in(uint16_t port);
void vga_out(uint16_t port, uint8_t v);
void vga_set_mode(int mode);
void vga_render(HostFrame *f);
void vga_debug_dump(const char *path);   /* the four planes, raw */
void vga_vsync_latch(void);
extern int g_vga_retrace;             /* 1 during vertical retrace */
extern int g_vga_hblank;

void sb_reset(void);
uint8_t sb_in(uint16_t port);
void sb_out(uint16_t port, uint8_t v);
void dma_out(uint16_t port, uint8_t v);
uint8_t dma_in(uint16_t port);
void sb_advance(uint64_t pit_ticks);    /* generate audio up to now */
uint64_t sb_next_event(void);            /* PIT tick of the next DMA completion, or UINT64_MAX */
void speaker_set(int on, uint16_t divisor);

void pic_raise(int irq);
void pic_lower(int irq);
uint64_t hal_now(void);                  /* virtual time in PIT ticks */
void hal_video_update(void);             /* g_vga_retrace, g_vga_hblank for now */
void hal_idle(void);                     /* a polling loop: skip to the next event */
void hal_program_exit(void);

void dos_init(void);
void dos_trap(Cpu *c, uint8_t n);

int sqz_unpack(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, int cdrun);

/* pack.c: game files held in memory (tools/mkpak.py); searched before the game folder */
int hal_set_pack(const uint8_t *pak, size_t len);   /* 1 when the pack is valid; it must stay alive */
int hal_have_pack(void);
const uint8_t *hal_pack_find(const char *name, size_t *len);

#endif
