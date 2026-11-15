/* machine.c - PIC, PIT, keyboard controller, the port map, the scheduler and the loader. */
#include "hal.h"
#include "state.h"
#include "../qol/qol.h"
#include "../gen/gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- virtual clock ---- */
uint64_t hal_now(void)
{
    uint64_t ic = g_cpu.icount;
    return (ic / HAL_IPS) * PIT_HZ + ((ic % HAL_IPS) * PIT_HZ) / HAL_IPS;
}
static uint64_t ticks_to_icount(uint64_t t)
{
    return (t / PIT_HZ) * HAL_IPS + ((t % PIT_HZ) * HAL_IPS + PIT_HZ - 1u) / PIT_HZ;
}

/* ---- 8259 PIC (master only) ---- */
static uint8_t pic_irr, pic_isr, pic_imr = 0xB8u, pic_icw_step;
static int pic_read_isr;

void pic_raise(int irq)
{
    pic_irr |= (uint8_t)(1u << irq);
    g_cpu.irq_pending = 1;
}
void pic_lower(int irq)
{
    pic_irr &= (uint8_t)~(1u << irq);
}
int g_hal_sandbox;      /* qol/wide.c runs the game's drawing code aside: no interrupts, no devices but the VGA */

int pic_ack(void)
{
    int i;
    uint8_t req;
    if (g_hal_sandbox) return -1;
    req = (uint8_t)(pic_irr & ~pic_imr);
    for (i = 0; i < 8; i++) {
        if (pic_isr & (1u << i)) return -1;        /* a higher or equal priority is in service */
        if (req & (1u << i)) {
            pic_irr &= (uint8_t)~(1u << i);
            pic_isr |= (uint8_t)(1u << i);
            return 8 + i;
        }
    }
    return -1;
}
static void kbd_eoi(void);
static void pic_eoi(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (pic_isr & (1u << i)) {
            pic_isr &= (uint8_t)~(1u << i);
            if (i == 1) kbd_eoi();
            break;
        }
    }
    g_cpu.irq_pending = 1;
}
static void pic_out(uint16_t port, uint8_t v)
{
    if (port == 0x20) {
        if (v & 0x10u) { pic_icw_step = 1; pic_imr = 0; pic_isr = 0; return; }   /* ICW1 */
        if ((v & 0x18u) == 0x08u) { pic_read_isr = (v & 3u) == 3u; return; }   /* OCW3 */
        if ((v & 0xE0u) == 0x20u) { pic_eoi(); return; }                        /* non-specific EOI */
        if ((v & 0xE0u) == 0x60u) {                                             /* specific EOI */
            pic_isr &= (uint8_t)~(1u << (v & 7u));
            if ((v & 7u) == 1u) kbd_eoi();
            g_cpu.irq_pending = 1;
        }
        return;
    }
    if (pic_icw_step) {                    /* ICW2..4: the vector base stays 8 */
        if (++pic_icw_step > 3) pic_icw_step = 0;
        return;
    }
    pic_imr = v;
    g_cpu.irq_pending = 1;
}
static uint8_t pic_in(uint16_t port)
{
    if (port == 0x20) return pic_read_isr ? pic_isr : pic_irr;
    return pic_imr;
}

/* ---- 8254 PIT ---- */
typedef struct PitCh {
    uint32_t reload;      /* 1..65536 */
    uint8_t mode, access, wstate, rstate;
    uint8_t lo;
    uint16_t latch;
    int latched;
    uint64_t start;       /* PIT tick when the current count started */
} PitCh;
static PitCh pit[3];
static uint64_t pit0_next = UINT64_MAX;
static uint8_t port61;

static uint16_t pit_count(int ch)
{
    PitCh *p = &pit[ch];
    uint64_t el = hal_now() - p->start;
    uint32_t n = p->reload;
    if (p->mode == 0) return (uint16_t)(el >= n ? 0xFFFFu - (el - n) : n - el);
    return (uint16_t)(n - (uint32_t)(el % n));
}
static void pit_load(int ch, uint32_t v)
{
    PitCh *p = &pit[ch];
    p->reload = v ? v : 65536u;
    p->start = hal_now();
    if (ch == 0) pit0_next = p->start + p->reload;
    if (ch == 2) speaker_set((port61 & 3u) == 3u, (uint16_t)p->reload);
}
static void pit_out(uint16_t port, uint8_t v)
{
    if (port == 0x43) {
        int ch = v >> 6;
        if (ch == 3) return;
        if (((v >> 4) & 3u) == 0) { pit[ch].latch = pit_count(ch); pit[ch].latched = 1; return; }
        pit[ch].access = (uint8_t)((v >> 4) & 3u);
        pit[ch].mode = (uint8_t)((v >> 1) & 7u);
        if (pit[ch].mode > 5) pit[ch].mode &= 3u;
        pit[ch].wstate = pit[ch].rstate = 0;
        return;
    }
    {
        int ch = port - 0x40;
        PitCh *p = &pit[ch];
        switch (p->access) {
        case 1: pit_load(ch, v); break;
        case 2: pit_load(ch, (uint32_t)v << 8); break;
        default:
            if (p->wstate == 0) { p->lo = v; p->wstate = 1; }
            else { p->wstate = 0; pit_load(ch, p->lo | ((uint32_t)v << 8)); }
            break;
        }
    }
}
static uint8_t pit_in(uint16_t port)
{
    int ch = port - 0x40;
    PitCh *p = &pit[ch];
    uint16_t v = p->latched ? p->latch : pit_count(ch);
    if (p->access == 1) { p->latched = 0; return (uint8_t)v; }
    if (p->access == 2) { p->latched = 0; return (uint8_t)(v >> 8); }
    if (p->rstate == 0) { p->rstate = 1; return (uint8_t)v; }
    p->rstate = 0;
    p->latched = 0;
    return (uint8_t)(v >> 8);
}

/* ---- keyboard ---- */
static uint8_t kq[64];
static int kq_head, kq_tail;
static uint8_t kbd_data;
static int kbd_full;            /* a byte waits in port 60h */
static uint64_t kbd_next = UINT64_MAX;
static uint8_t key_held[256];   /* the keys the machine has seen go down and not up (scancode | ext << 7) */

void hal_key(uint8_t sc, int down, int ext)
{
    int n = (kq_tail + (ext ? 2 : 1)) % 64;
    key_held[(sc & 0x7Fu) | (ext ? 0x80u : 0u)] = (uint8_t)(down != 0);
    if ((kq_tail + 1) % 64 == kq_head || n == kq_head) return;
    if (ext) { kq[kq_tail] = 0xE0u; kq_tail = (kq_tail + 1) % 64; }
    kq[kq_tail] = (uint8_t)(down ? sc : (sc | 0x80u));
    kq_tail = (kq_tail + 1) % 64;
    if (kbd_next == UINT64_MAX && !kbd_full) kbd_next = hal_now();
}
static void kbd_deliver(void)
{
    kbd_next = UINT64_MAX;
    if (kbd_full || kq_head == kq_tail) return;
    kbd_data = kq[kq_head];
    kq_head = (kq_head + 1) % 64;
    kbd_full = 1;
    pic_raise(1);
}
static void kbd_eoi(void)
{
    kbd_full = 0;
    if (kq_head != kq_tail) kbd_next = hal_now() + 600u;   /* about 0.5 ms later */
}

/* ---- port map ---- */
static int g_port_log;

uint8_t io_in8(uint16_t port)
{
    if (port >= 0x3B0 && port <= 0x3DF) return vga_in(port);
    if (g_hal_sandbox) return 0xFFu;
    if (port >= 0x210 && port <= 0x26F) return sb_in(port);
    if (port < 0x20 || (port >= 0x80 && port <= 0x8F)) return dma_in(port);
    switch (port) {
    case 0x20: case 0x21: return pic_in(port);
    case 0x40: case 0x41: case 0x42: return pit_in(port);
    case 0x60: kbd_full = kbd_full; return kbd_data;
    case 0x61: {
        uint8_t v = (uint8_t)(port61 & 0x0Fu);
        if ((hal_now() / 18u) & 1u) v |= 0x10u;            /* refresh toggle, 15 us */
        if (pit[2].reload && ((hal_now() - pit[2].start) % pit[2].reload) < pit[2].reload / 2u) v |= 0x20u;
        return v;
    }
    case 0x64: return (uint8_t)(0x14u | (kbd_full ? 1u : 0u));
    case 0x201: return 0xF0u;          /* no joystick: the one-shots never fire */
    case 0x388: case 0x389: return 0xFFu;   /* no AdLib */
    default:
        if (g_port_log) fprintf(stderr, "in %04x at %04x:%04x\n", port, g_cpu.op_cs, g_cpu.op_ip);
        return 0xFFu;
    }
}

void io_out8(uint16_t port, uint8_t v)
{
    if (port >= 0x3B0 && port <= 0x3DF) { vga_out(port, v); return; }
    if (g_hal_sandbox) return;
    if (port >= 0x210 && port <= 0x26F) { sb_out(port, v); return; }
    if (port < 0x20 || (port >= 0x80 && port <= 0x8F)) { dma_out(port, v); return; }
    switch (port) {
    case 0x20: case 0x21: pic_out(port, v); return;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_out(port, v); return;
    case 0x61:
        port61 = v;
        speaker_set((v & 3u) == 3u, (uint16_t)pit[2].reload);
        return;
    case 0x64: case 0x60: case 0x388: case 0x389: case 0x3F2: case 0x201: return;
    default:
        if (g_port_log) fprintf(stderr, "out %04x,%02x at %04x:%04x\n", port, v, g_cpu.op_cs, g_cpu.op_ip);
        return;
    }
}

/* ---- the scheduler ----
 * Frame k spans [frame_tick(k), frame_tick(k+1)): 400 displayed lines of 449, the vertical retrace
 * at lines 412..413.  The CRTC start address is latched when the retrace begins; the picture is
 * built at the end of the frame (the next frame's display start), with the panning and memory
 * the program set during the retrace and blanking. */
#define FRAME_TICKS_NUM (PIT_HZ * 1000u)
#define FRAME_TICKS_DEN 70086u           /* 70.086 Hz */
static uint64_t frame_no, frame_begin, retrace_on, retrace_off;
static int latched_this_frame;
static uint64_t real_base, virt_base;
static int g_exited;
static int g_turbo, g_rebase;      /* fast forward; the clocks are set together again at the next frame */
static uint32_t g_prog_hash;
static uint64_t g_run_until;
char g_hal_exit_msg[256];

int hal_exited(void) { return g_exited; }
void hal_set_turbo(int on) { g_rebase |= g_turbo != on; g_turbo = on; }
uint32_t hal_program_hash(void) { return g_prog_hash; }
void hal_program_exit(void) { g_exited = 1; g_cpu.stop = 1; }

static uint64_t frame_tick(uint64_t n)
{
    return (n * FRAME_TICKS_NUM) / FRAME_TICKS_DEN;
}

/* a polling loop on 3DAh skips ahead to the next event */
void hal_idle(void)
{
    if (g_hal_sandbox) return;
    if (g_run_until > g_cpu.icount) g_cpu.icount = g_run_until;
}

void hal_video_update(void)
{
    uint64_t now = hal_now();
    uint64_t len = frame_tick(frame_no + 1) - frame_begin;
    uint64_t in_frame = now >= frame_begin ? now - frame_begin : 0;
    uint64_t line = len / 449u;
    g_vga_retrace = now >= retrace_on && now < retrace_off;
    g_vga_hblank = in_frame >= line * 400u || (line && (in_frame % line) >= line * 4u / 5u);
}

int hal_frame(void)
{
    uint64_t end;
    HostFrame f;
    if (g_exited) return 0;
    frame_begin = frame_tick(frame_no);
    end = frame_tick(frame_no + 1);
    retrace_on = frame_begin + (end - frame_begin) * 412u / 449u;
    retrace_off = frame_begin + (end - frame_begin) * 414u / 449u;
    latched_this_frame = 0;
    for (;;) {
        uint64_t now = hal_now(), next = end;
        uint64_t t;
        if (pit0_next <= now) {
            uint64_t per = pit[0].reload;
            pic_raise(0);
            if (pit[0].mode == 0) pit0_next = UINT64_MAX;
            else {
                pit0_next += per;
                if (pit0_next <= now) pit0_next = now + per;
            }
        }
        if (!latched_this_frame && now >= retrace_on) {
            vga_vsync_latch();
            latched_this_frame = 1;
        }
        if (kbd_next <= now) kbd_deliver();
        sb_advance(now);
        if (now >= end) break;
        if (pit0_next < next) next = pit0_next;
        if (kbd_next < next) next = kbd_next;
        t = sb_next_event();
        if (t < next) next = t;
        if (now < retrace_on && retrace_on < next) next = retrace_on;
        else if (now < retrace_off && retrace_off < next) next = retrace_off;
        if (next <= now) next = now + 1u;
        g_run_until = ticks_to_icount(next);
        cpu_run(&g_cpu, g_run_until);
        if (g_cpu.stop) {
            g_cpu.stop = 0;
            if (g_exited) return 0;
        }
    }
    frame_no++;
    if (g_rebase) {
        real_base = host_time_us();
        virt_base = end;
        g_rebase = 0;
    }
    if (!g_turbo || (frame_no & 3u) == 0) {         /* fast forward: four times the speed, every fourth picture */
        uint64_t virt_us, real;
        vga_render(&f);
        qol_wide_frame(&f);
        host_present(&f);
        virt_us = (end - virt_base) * 1000000u / PIT_HZ;
        if (g_turbo) virt_us /= 4u;
        real = host_time_us() - real_base;
        if (real < virt_us) host_sleep_us(virt_us - real);
        else if (real > virt_us + 100000u) {
            real_base = host_time_us();
            virt_base = end;
        }
    }
    host_pump();
    return !host_quit_requested();
}

/* ---- the loader ---- */
static uint8_t *read_file(const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    uint8_t *b;
    long n;
    const uint8_t *m = hal_pack_find(name, len);
    if (m != NULL) {
        b = (uint8_t *)malloc(*len > 0 ? *len : 1u);
        if (b != NULL) memcpy(b, m, *len);
        return b;
    }
    (void)snprintf(path, sizeof path, "%s/%s", host_game_dir(), name);
    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    (void)fseek(f, 0, SEEK_END);
    n = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(n > 0 ? (size_t)n : 1u);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void bios_init(void)
{
    int i;
    /* the trap stubs: F000:(n*4) = F1 n CF; IVT n -> F000:n*4 */
    for (i = 0; i < 256; i++) {
        uint32_t a = TRAP_SEG * 16u + (uint32_t)i * 4u;
        g_mem[a] = TRAP_OPCODE;
        g_mem[a + 1] = (uint8_t)i;
        g_mem[a + 2] = 0xCF;
        g_mem[a + 3] = 0x90;
        g_mem[i * 4] = (uint8_t)(i * 4);
        g_mem[i * 4 + 1] = (uint8_t)((i * 4) >> 8);
        g_mem[i * 4 + 2] = (uint8_t)TRAP_SEG;
        g_mem[i * 4 + 3] = (uint8_t)(TRAP_SEG >> 8);
    }
    g_mem[0x410] = 0x26; g_mem[0x411] = 0x00;     /* equipment: 80x25 colour, no FPU */
    g_mem[0x413] = 0x80; g_mem[0x414] = 0x02;     /* 640 KB */
    g_mem[0x449] = 0x03;
    g_mem[0x44A] = 80;
    g_mem[0x463] = 0xD4; g_mem[0x464] = 0x03;
    g_mem[0x484] = 24;
    g_mem[0xFFFFE] = 0xFC;                         /* AT */
}

int hal_init(char *err, int errlen)
{
    size_t n, xn;
    uint8_t *sqz = read_file("PRE2.SQZ", &n), *exe = NULL;
    uint16_t cblp, cp, crlc, hdr, ss, sp, ip, cs, lfarlc, i;
    size_t size;
    memset(g_mem, 0, sizeof g_mem);
    cpu_reset(&g_cpu);
    if (sqz == NULL) {
        (void)snprintf(err, (size_t)errlen, "PRE2.SQZ was not found in\n%s", host_game_dir());
        return 0;
    }
    if (!sqz_unpack(sqz, n, &exe, &xn, 1) || xn < 0x40 || exe[0] != 'M' || exe[1] != 'Z') {
        free(sqz);
        free(exe);
        (void)snprintf(err, (size_t)errlen, "PRE2.SQZ is damaged or not the expected version.");
        return 0;
    }
    free(sqz);
    cblp = (uint16_t)(exe[2] | exe[3] << 8);
    cp = (uint16_t)(exe[4] | exe[5] << 8);
    crlc = (uint16_t)(exe[6] | exe[7] << 8);
    hdr = (uint16_t)((exe[8] | exe[9] << 8) * 16);
    ss = (uint16_t)(exe[0x0E] | exe[0x0F] << 8);
    sp = (uint16_t)(exe[0x10] | exe[0x11] << 8);
    ip = (uint16_t)(exe[0x14] | exe[0x15] << 8);
    cs = (uint16_t)(exe[0x16] | exe[0x17] << 8);
    lfarlc = (uint16_t)(exe[0x18] | exe[0x19] << 8);
    size = (size_t)(cp - 1u) * 512u + (cblp ? cblp : 512u) - hdr;
    if (hdr + size > xn) { free(exe); (void)snprintf(err, (size_t)errlen, "PRE2.EXE is truncated."); return 0; }
    bios_init();
    memcpy(g_mem + LOAD_SEG * 16u, exe + hdr, size);
    {
        size_t k;
        g_prog_hash = 2166136261u;
        for (k = 0; k < size; k++) g_prog_hash = (g_prog_hash ^ exe[hdr + k]) * 16777619u;
    }
    for (i = 0; i < crlc; i++) {
        uint16_t off = (uint16_t)(exe[lfarlc + i * 4] | exe[lfarlc + i * 4 + 1] << 8);
        uint16_t seg = (uint16_t)(exe[lfarlc + i * 4 + 2] | exe[lfarlc + i * 4 + 3] << 8);
        uint32_t a = (uint32_t)(LOAD_SEG + seg) * 16u + off;
        uint16_t v = (uint16_t)(g_mem[a] | g_mem[a + 1] << 8);
        v = (uint16_t)(v + LOAD_SEG);
        g_mem[a] = (uint8_t)v;
        g_mem[a + 1] = (uint8_t)(v >> 8);
    }
    free(exe);
    /* the PSP */
    {
        uint32_t p = PSP_SEG * 16u;
        g_mem[p] = 0xCD; g_mem[p + 1] = 0x20;
        g_mem[p + 2] = 0x00; g_mem[p + 3] = 0xA0;      /* top of memory A000h */
        g_mem[p + 0x80] = 0;
        g_mem[p + 0x81] = 0x0D;
    }
    g_cpu.s[CS] = (uint16_t)(LOAD_SEG + cs);
    g_cpu.ip = ip;
    g_cpu.s[SS] = (uint16_t)(LOAD_SEG + ss);
    g_cpu.r[SP] = sp;
    g_cpu.s[DS] = g_cpu.s[ES] = PSP_SEG;
    g_cpu.flags = 0x0202u;
    vga_reset();
    sb_reset();
    dos_init();
    pit_load(0, 0);
    pit[0].mode = 3;
    pit[0].access = 3;
    real_base = host_time_us();
    virt_base = 0;
    frame_no = 0;
    g_exited = 0;
    g_hal_exit_msg[0] = 0;
    kq_head = kq_tail = 0;
    kbd_full = 0;
    kbd_next = UINT64_MAX;
    memset(key_held, 0, sizeof key_held);
    g_port_log = getenv("P2_PORTLOG") != NULL;
    gen_install(getenv("P2_INTERP") == NULL);   /* P2_INTERP: the interpreter alone (tests) */
    qol_init();
    return 1;
}

/* ---- state ---- */
void machine_state(StateIO *s)
{
    ST(s, pic_irr); ST(s, pic_isr); ST(s, pic_imr); ST(s, pic_icw_step); ST(s, pic_read_isr);
    ST(s, pit); ST(s, pit0_next); ST(s, port61);
    ST(s, kq); ST(s, kq_head); ST(s, kq_tail); ST(s, kbd_data); ST(s, kbd_full); ST(s, kbd_next);
    ST(s, key_held);
    ST(s, frame_no); ST(s, frame_begin); ST(s, retrace_on); ST(s, retrace_off); ST(s, latched_this_frame);
    ST(s, g_exited);
    ST(s, g_hal_exit_msg);
}

/* after a state was loaded: real time starts over, and the keys that were held when the state was
 * saved are released (the host presses again what is held now) */
void hal_state_loaded(void)
{
    int k;
    real_base = host_time_us();
    virt_base = frame_tick(frame_no);
    g_cpu.stop = 0;
    for (k = 0; k < 256; k++)
        if (key_held[k]) hal_key((uint8_t)(k & 0x7F), 0, (k & 0x80) != 0);
}

void hal_trap(Cpu *c, uint8_t n)
{
    if (g_hal_sandbox) { qol_sandbox_trap(c, n); return; }
    switch (n) {
    case 0x08: {                    /* the BIOS timer: ticks at 0040:006C, then EOI */
        uint32_t t = (uint32_t)(g_mem[0x46C] | g_mem[0x46D] << 8 | g_mem[0x46E] << 16 | (uint32_t)g_mem[0x46F] << 24);
        t++;
        g_mem[0x46C] = (uint8_t)t; g_mem[0x46D] = (uint8_t)(t >> 8);
        g_mem[0x46E] = (uint8_t)(t >> 16); g_mem[0x46F] = (uint8_t)(t >> 24);
        pic_eoi();
        return;
    }
    case 0x09:
        kbd_full = 0;
        pic_eoi();
        return;
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        pic_eoi();
        return;
    default:
        dos_trap(c, n);
        return;
    }
}
