/* wide.c - widescreen: a playfield wider than the original's 320 pixels, drawn by the game's own code.
 *
 * How the game draws (code segment 0020 of PRE2.EXE; all offsets below are in it or in data segment 09FF):
 *   - the level is a map of 256 x n tiles of 16x16 pixels (segment [2DDA], one byte per tile);
 *   - the camera is at tile [2DE4],[2DE6] (plus [6BC4] pixels down); it moves sideways by whole tiles;
 *   - a buffer of 20x12 tiles in video memory (3F40h, 40 bytes a row, used as a torus whose origin is
 *     [2DE8],[2DEA]/[2DBA]) holds the background of exactly one screen; tiles with "see-through" parts
 *     show the backdrop picture stored at 7E80h, which does not scroll sideways;
 *   - every frame 3A27 copies that buffer to the hidden page [2DD8], then 4B8E (sparks), 26FA (the
 *     sprite list, world coordinates minus the camera), 3721 (tiles in front of sprites), 54AB and
 *     3922 (insects, snow/rain) draw over it, and 4509 shows the page.
 * So there is nothing beside the 320 pixels to show.  To get it, the frame is drawn again with the
 * camera moved: just before the game draws its sprites, this module keeps a copy of the machine
 * (memory, registers, video memory), moves the camera a few tiles to the left, has the game's own
 * routines fill the tile buffer, compose the page and draw the sprites, and keeps the columns that
 * lie left of the real picture; then the same to the right; then it puts the machine back.  The
 * game never notices: its state, its timing and the 320 pixels it draws are those of the original.
 * The margins are stored as pixel values, not colours, so palette fades apply to them as well.
 *
 * The margins stay inside the level: at the left edge of the map, and at the camera's right limit
 * (the one the game's own scroll routine 3435 uses), the wide picture stops and the 320-pixel
 * picture moves off centre instead.  The backdrop is continued by mirroring it at its edges.
 * Screens that are not the playfield (menus, the map, the bonus screens) stay 4:3. */
#include "qol.h"
#include "../gen/gen.h"
#include "../hal/state.h"
#include <string.h>

#define DS_SEG REL(0x9FFu)
#define DS_LIN ((uint32_t)DS_SEG * 16u)
#define VIEW_H 176                     /* the playfield's rows; the status bar is below */
#define ROW_BYTES 40
#define BUF_ORG 0x3F40u                /* the tile buffer */
#define BACKDROP 0x7E80u               /* the backdrop picture: 192 rows of 40 bytes */
#define BACKDROP_ROWS 192
#define SBX_RET 0x0272u                /* a gap between routines: the calls made here return to a trap planted there */
#define SBX_TRAP 0xFEu
#define SBX_BUDGET 3000000u            /* instructions one call may take */

/* routines */
#define FN_WRITE_MODE1 0x453Bu         /* latch copy, all planes */
#define FN_TILE 0x3B7Bu                /* AL = tile, DI = address in the buffer */
#define FN_SPECIAL_TILES 0x35A1u
#define FN_COMPOSE 0x3A27u
#define FN_SPARKS 0x4B8Eu
#define FN_SPRITES 0x26FAu
#define FN_FRONT_TILES 0x3721u
#define FN_INSECTS 0x54ABu
#define FN_WEATHER 0x3922u
/* hooks */
#define AT_LOOP 0x024Au                /* main loop, after 3A27 */
#define AT_DEATH 0x50FDu               /* the losing-a-life loop, after 3A27 */
#define AT_MOVE 0x53DDu                /* after a passage moved the player, after 3A27 */
#define AT_START 0x3B71u               /* level start, after 3A27 */
#define AT_CURTAIN 0x3054u             /* the hidden page is revealed column by column on the shown one */
#define AT_CURTAIN_END 0x309Au
#define AT_FLIP 0x4509u

typedef struct Margins {
    int valid, fresh;
    unsigned serial;                   /* g_vga_mode_serial when drawn */
    int ml, mr;                        /* pixels left and right of the game's own picture */
    uint8_t l[VIEW_H * (WIDE_MAX - 320)], r[VIEW_H * (WIDE_MAX - 320)];   /* pixel values, ml / mr a row */
} Margins;

static int g_supported, g_width = WIDE_OFF;
static Margins g_page[2];              /* of the pages at 0000h and 2000h */
static uint32_t g_wfb[WIDE_MAX * 200];

/* the copy of the machine */
static Cpu s_cpu;
static uint8_t s_mem[0xA0000];
static uint8_t s_vga[4 * 65536 + 4096];
static size_t s_vga_len;
static int sbx_done, sbx_fail;

static uint16_t dsw(uint16_t off) { return (uint16_t)(g_mem[DS_LIN + off] | (g_mem[DS_LIN + off + 1u] << 8)); }
static uint8_t dsb(uint16_t off) { return g_mem[DS_LIN + off]; }
static void set_dsw(uint16_t off, uint16_t v) { g_mem[DS_LIN + off] = (uint8_t)v; g_mem[DS_LIN + off + 1u] = (uint8_t)(v >> 8); }
static void set_dsb(uint16_t off, uint8_t v) { g_mem[DS_LIN + off] = v; }

int qol_wide_supported(void) { return g_supported; }
int qol_wide_width(void) { return g_width; }

void qol_set_wide(int width)
{
    if (width != WIDE_16_10 && width != WIDE_16_9 && width != WIDE_21_9) width = WIDE_OFF;
    g_width = width;
    g_page[0].valid = g_page[1].valid = 0;
}

void qol_reset(void)
{
    g_page[0].valid = g_page[1].valid = 0;
}

/* The margins belong to a saved state, so that the first picture after loading is whole.  They are
 * of use only when the width is still the one they were drawn for. */
void qol_state(StateIO *s)
{
    int width = g_width, i;
    ST(s, width);
    ST(s, g_page);
    if (s->write) return;
    for (i = 0; i < 2; i++) {
        Margins *m = &g_page[i];
        if (s->err || width != g_width || m->ml < 0 || m->mr < 0 || m->ml + m->mr > g_width - 320) m->valid = m->fresh = 0;
        m->serial = g_vga_mode_serial;
    }
}

/* ---- the machine's copy ---- */
static void snapshot(void)
{
    StateIO s;
    s_cpu = g_cpu;
    memcpy(s_mem, g_mem, sizeof s_mem);
    memset(&s, 0, sizeof s);
    s.write = 1;
    s.buf = s_vga;
    s.cap = sizeof s_vga;
    s.fixed = 1;
    vga_state(&s);
    s_vga_len = s.err ? 0 : s.pos;
}

static void restore(void)
{
    StateIO s;
    unsigned serial = g_vga_mode_serial;
    g_cpu = s_cpu;
    memcpy(g_mem, s_mem, sizeof s_mem);
    memset(&s, 0, sizeof s);
    s.buf = s_vga;
    s.cap = s_vga_len;
    s.fixed = 1;
    vga_state(&s);
    g_vga_mode_serial = serial;        /* the same video mode as before: nothing became stale */
}

void qol_sandbox_trap(Cpu *c, uint8_t n)
{
    if (n == SBX_TRAP) sbx_done = 1;
    else sbx_fail = 1;                 /* a DOS or BIOS call: not something a drawing routine does */
    c->stop = 1;
}

/* a near call of a routine of the game; 1 when it returned */
static int sbx_call(uint16_t fn)
{
    Cpu *c = &g_cpu;
    uint64_t budget = c->icount + SBX_BUDGET;
    if (sbx_fail) return 0;
    c->s[DS] = c->s[ES] = DS_SEG;
    c->s[CS] = GAME_CS;
    c->flags &= (uint16_t)~F_DF;
    push16(c, SBX_RET);
    c->ip = fn;
    sbx_done = 0;
    c->stop = 0;
    c->halted = 0;
    while (!sbx_done && !sbx_fail) {
        if (c->icount >= budget || c->halted) { sbx_fail = 1; break; }
        cpu_run(c, budget);
        c->stop = 0;
    }
    return sbx_done && !sbx_fail;
}

static uint8_t bitrev(uint8_t b)
{
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    return (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
}

/* the backdrop as it continues beside the screen: mirrored, and moved by the camera's shift, so the
 * part that the kept columns show is the mirror image of the backdrop's edge */
static void shift_backdrop(int tiles)
{
    int p, r, k;
    for (p = 0; p < 4; p++) {
        uint8_t *pl = vga_plane(p);
        for (r = 0; r < BACKDROP_ROWS; r++) {
            uint8_t *row = pl + BACKDROP + (size_t)r * ROW_BYTES, m[ROW_BYTES];
            for (k = 0; k < ROW_BYTES; k++) m[k] = bitrev(row[ROW_BYTES - 1 - k]);
            for (k = 0; k < ROW_BYTES; k++) row[k] = m[((k + 2 * tiles) % ROW_BYTES + ROW_BYTES) % ROW_BYTES];
        }
    }
}

/* draws the frame with the camera `tiles` to the side and keeps n columns from column c0 on */
static int draw_side(int tiles, const uint16_t *fns, int c0, int n, uint8_t *out)
{
    uint16_t cx = (uint16_t)(dsw(0x2DE4) + tiles), cy = dsw(0x2DE6), page = dsw(0x2DD8);
    uint32_t map = (uint32_t)dsw(0x2DDA) * 16u;
    uint16_t anim = dsw(0x6BC2);
    int row, col, x;
    const uint8_t *pl[4];
    g_hal_sandbox = 1;
    g_cpu_trap_any = 1;
    sbx_fail = 0;
    g_mem[GAME_CODE_LIN + SBX_RET] = TRAP_OPCODE;
    g_mem[GAME_CODE_LIN + SBX_RET + 1u] = SBX_TRAP;
    set_dsw(0x2DE4, cx);
    set_dsw(0x2DE8, 0);                /* the tile buffer starts over, origin at its first cell */
    set_dsw(0x2DEA, 0);
    set_dsw(0x2DBA, BUF_ORG);
    shift_backdrop(tiles);
    (void)sbx_call(FN_WRITE_MODE1);
    for (row = 0; row < 12 && !sbx_fail; row++) {
        for (col = 0; col < 20 && !sbx_fail; col++) {
            uint8_t t = g_mem[(map + ((((uint32_t)cy + (uint32_t)row) & 0xFFu) << 8) + ((cx + (uint32_t)col) & 0xFFu)) & 0xFFFFFu];
            if (dsb((uint16_t)(0x6988u + t)) != 0) t = dsb((uint16_t)(anim + t));   /* an animated tile's present picture (3668) */
            set_dsw(0x2DF6, (uint16_t)(BACKDROP + row * 0x280 + col * 2));
            g_cpu.r[AX] = t;
            g_cpu.r[DI] = (uint16_t)(BUF_ORG + row * 0x280 + col * 2);
            (void)sbx_call(FN_TILE);
        }
    }
    set_dsb(0x2DF4, 1);                /* as at a level's start (3B60): every special tile again, flags recomputed */
    set_dsw(0x2DE0, 0x55AA);
    (void)sbx_call(FN_SPECIAL_TILES);
    (void)sbx_call(FN_COMPOSE);
    for (; *fns != 0; fns++) (void)sbx_call(*fns);
    g_hal_sandbox = 0;
    g_cpu_trap_any = 0;
    if (sbx_fail) return 0;
    for (x = 0; x < 4; x++) pl[x] = vga_plane(x);
    for (row = 0; row < VIEW_H; row++) {
        uint16_t base = (uint16_t)(page + row * ROW_BYTES);
        for (x = 0; x < n; x++) {
            int px = c0 + x, bit = 7 - (px & 7);
            uint16_t a = (uint16_t)(base + (px >> 3));
            out[row * n + x] = (uint8_t)(((pl[0][a] >> bit) & 1) | (((pl[1][a] >> bit) & 1) << 1) |
                                         (((pl[2][a] >> bit) & 1) << 2) | (((pl[3][a] >> bit) & 1) << 3));
        }
    }
    return 1;
}

/* the margins of the frame the game is drawing on the hidden page */
static void draw_margins(const uint16_t *fns)
{
    uint16_t page = dsw(0x2DD8);
    Margins *m;
    int x, lim, m_all, ml, mr, ok = 1;
    if (page != 0 && page != 0x2000u) return;
    m = &g_page[page != 0];
    m->valid = 0;
    if (g_width <= 320 || !vga_is_320x200x16()) return;
    if (g_mem[GAME_CODE_LIN + FN_SPRITES] != 0x50u) return;      /* the program has overwritten its sprite routine */
    x = dsw(0x2DE4);
    /* the camera's right limit, as 3435 has it: [8164], or the map's end once the player is past it */
    lim = (int16_t)dsw(0x8164);
    if (lim < ((int16_t)dsw(0x4F1C) >> 4) - 20) lim = 0xEC;
    if (lim > 0xEC) lim = 0xEC;
    if (lim < x) lim = x;
    if (x > 0xEC) return;
    m_all = g_width - 320;
    ml = m_all / 2;
    if (ml > x * 16) ml = x * 16;
    mr = m_all - ml;
    if (mr > (lim - x) * 16) mr = (lim - x) * 16;
    if (ml < m_all - mr) ml = m_all - mr;
    if (ml > x * 16) ml = x * 16;
    snapshot();
    if (s_vga_len == 0) return;
    if (ml > 0) {
        int d = (ml + 15) / 16;
        ok = draw_side(-d, fns, d * 16 - ml, ml, m->l);
        restore();
    }
    if (ok && mr > 0) {
        int d = (mr + 15) / 16;
        ok = draw_side(d, fns, 320 - d * 16, mr, m->r);
        restore();
    }
    if (!ok) return;
    m->ml = ml;
    m->mr = mr;
    m->serial = g_vga_mode_serial;
    m->valid = m->fresh = 1;
}

static const uint16_t g_fns_loop[] = {FN_SPARKS, FN_SPRITES, FN_FRONT_TILES, FN_INSECTS, FN_WEATHER, 0};
static const uint16_t g_fns_move[] = {FN_SPRITES, FN_FRONT_TILES, FN_INSECTS, FN_WEATHER, 0};
static const uint16_t g_fns_none[] = {0};

static void ip_hook(Cpu *c)
{
    static uint64_t last_ic;
    static uint16_t last_ip;
    if (g_hal_sandbox || c->s[CS] != GAME_CS) return;
    if (c->icount == last_ic && c->ip == last_ip) return;     /* a block that declined: the interpreter asks again */
    last_ic = c->icount;
    last_ip = c->ip;
    switch (c->ip) {
    case AT_LOOP: case AT_DEATH: draw_margins(g_fns_loop); break;
    case AT_MOVE: draw_margins(g_fns_move); break;
    case AT_START: draw_margins(g_fns_none); break;
    case AT_CURTAIN: {                  /* the shown page is being drawn over: its margins go */
        uint16_t front = dsw(0x2DD6);
        if (front == 0 || front == 0x2000u) g_page[front != 0].valid = 0;
        break;
    }
    case AT_CURTAIN_END: {              /* the shown page now holds the hidden page's picture */
        uint16_t front = dsw(0x2DD6), back = dsw(0x2DD8);
        if ((front == 0 || front == 0x2000u) && (back == 0 || back == 0x2000u) && front != back &&
            g_page[back != 0].valid && g_page[back != 0].fresh) {
            g_page[front != 0] = g_page[back != 0];
            g_page[front != 0].fresh = 0;
        }
        break;
    }
    case AT_FLIP: {                     /* the hidden page is shown: margins not drawn for this frame are stale */
        uint16_t back = dsw(0x2DD8);
        if (back == 0 || back == 0x2000u) {
            Margins *m = &g_page[back != 0];
            if (!m->fresh) m->valid = 0;
            m->fresh = 0;
        }
        break;
    }
    default: break;
    }
}

void qol_init(void)
{
    static const struct { uint16_t ip; uint8_t n; uint8_t b[9]; } sig[] = {
        {0x0247, 9, {0xE8, 0xDD, 0x37, 0xE8, 0x41, 0x49, 0xE8, 0xAA, 0x24}},
        {0x50FA, 9, {0xE8, 0x2A, 0xE9, 0xE8, 0x8E, 0xFA, 0xE8, 0xF7, 0xD5}},
        {0x53DA, 6, {0xE8, 0x4A, 0xE6, 0xE8, 0x27, 0xF5}},
        {0x3B6E, 6, {0xE8, 0xB6, 0xFE, 0xE8, 0xE0, 0xF4}},
        {0x3097, 4, {0xE8, 0x91, 0x14, 0xC3}},
        {FN_TILE, 5, {0x52, 0x06, 0xBA, 0x00, 0xA0}},
        {AT_FLIP, 7, {0x50, 0x53, 0x52, 0x1E, 0xBA, 0xD4, 0x03}},
        {FN_SPECIAL_TILES, 3, {0x50, 0x53, 0x51}},
        {FN_COMPOSE, 3, {0x53, 0x57, 0x56}},
        {FN_SPRITES, 3, {0x50, 0x53, 0x51}},
        {FN_WRITE_MODE1, 5, {0x50, 0x52, 0xBA, 0xCE, 0x03}},
        {FN_FRONT_TILES, 4, {0x2E, 0xF7, 0x06, 0x03}},
        {FN_INSECTS, 3, {0xBE, 0xA9, 0x6E}},
        {FN_WEATHER, 4, {0xFF, 0x06, 0xBE, 0x2D}},
        {FN_SPARKS, 4, {0x06, 0xE8, 0x99, 0xF9}},
    };
    static const uint16_t hooks[] = {AT_LOOP, AT_DEATH, AT_MOVE, AT_START, AT_CURTAIN, AT_CURTAIN_END, AT_FLIP};
    size_t i;
    g_supported = !gen_has_block(SBX_RET) && !gen_has_block(SBX_RET + 1u);
    for (i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (memcmp(g_mem + GAME_CODE_LIN + sig[i].ip, sig[i].b, sig[i].n) != 0) g_supported = 0;
    memset(g_ip_hook, 0, sizeof g_ip_hook);
    g_ip_hook_fn = NULL;
    qol_reset();
    if (!g_supported) return;
    g_ip_hook_cs = GAME_CS;
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; i++) g_ip_hook[hooks[i]] = 1;
    g_ip_hook_fn = ip_hook;
}

/* ---- the wide picture ---- */
void qol_wide_frame(HostFrame *f)
{
    const Margins *m;
    uint16_t start = vga_display_start();
    uint32_t pal[16];
    int w = g_width, ox, hx, row, x;
    if (w <= 320 || f->width != 320 || f->height != 200 || !vga_is_320x200x16()) return;
    if (start != 0 && start != 0x2000u) return;
    m = &g_page[start != 0];
    if (!m->valid || m->serial != g_vga_mode_serial) return;
    vga_colors16(pal);
    ox = m->ml + (w - 320 - m->ml - m->mr) / 2;      /* where the game's own picture goes */
    hx = (w - 320) / 2;                               /* the status bar stays in the middle */
    for (row = 0; row < VIEW_H; row++) {
        uint32_t *out = g_wfb + (size_t)row * (size_t)w;
        const uint8_t *l = m->l + (size_t)row * (size_t)m->ml, *r = m->r + (size_t)row * (size_t)m->mr;
        for (x = 0; x < ox - m->ml; x++) out[x] = 0;
        for (x = 0; x < m->ml; x++) out[ox - m->ml + x] = pal[l[x]];
        memcpy(out + ox, f->pixels + (size_t)row * 320u, 320u * sizeof(uint32_t));
        for (x = 0; x < m->mr; x++) out[ox + 320 + x] = pal[r[x]];
        for (x = ox + 320 + m->mr; x < w; x++) out[x] = 0;
    }
    for (row = VIEW_H; row < 200; row++) {
        /* the status bar's outer columns are plain: its ends are continued with them */
        uint32_t *out = g_wfb + (size_t)row * (size_t)w;
        const uint32_t *src = f->pixels + (size_t)row * 320u;
        memcpy(out + hx, src, 320u * sizeof(uint32_t));
        for (x = 0; x < hx; x++) {
            out[x] = src[1];
            out[hx + 320 + x] = src[318];
        }
        for (x = hx + 320 + hx; x < w; x++) out[x] = 0;
    }
    f->pixels = g_wfb;
    f->width = w;
    f->aspect_w = w;
    f->aspect_h = 240;
}
