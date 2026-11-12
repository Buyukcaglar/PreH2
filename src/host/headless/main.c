/* headless/main.c - runs the game without a window, for tests.
 *   p2test GAMEDIR FRAMES [OUTDIR] [script]
 * script: comma-separated "frame:key:down" events (key = set-1 scancode, hex), e.g. "300:1c:1,305:1c:0".
 * Writes OUTDIR/fNNNNN.png every 50 frames and at the end; prints a hash of each frame. */
#include "../../hal/hal.h"
#include "../pngw.h"
#include "../../gen/gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static const char *g_dir;
static const char *g_out;
static long g_frame;
static int g_every = 50;

const char *host_game_dir(void) { return g_dir; }
static FILE *g_wav;
static long g_wav_frames;
void host_audio(const int16_t *s, int n)
{
    if (g_wav) { fwrite(s, 4, (size_t)n, g_wav); g_wav_frames += n; }
}
static void wav_close(void)
{
    uint32_t hdr[11];
    if (!g_wav) return;
    hdr[0] = 0x46464952u; hdr[1] = 36u + (uint32_t)g_wav_frames * 4u; hdr[2] = 0x45564157u; hdr[3] = 0x20746D66u;
    hdr[4] = 16; hdr[5] = 0x00020001u; hdr[6] = 44100; hdr[7] = 44100 * 4; hdr[8] = 0x00100004u;
    hdr[9] = 0x61746164u; hdr[10] = (uint32_t)g_wav_frames * 4u;
    fseek(g_wav, 0, SEEK_SET);
    fwrite(hdr, 4, 11, g_wav);
    fclose(g_wav);
}
static uint64_t g_fake_us;
uint64_t host_time_us(void) { return g_fake_us; }
void host_sleep_us(uint64_t us) { g_fake_us += us; }
void host_pump(void) {}
int host_quit_requested(void) { return 0; }
void hal_fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s (frame %ld, cs:ip %04x:%04x)\n", msg, g_frame, g_cpu.op_cs, g_cpu.op_ip);
    exit(2);
}
static FILE *g_hashes;   /* P2_HASH: one line per frame, FNV-1a of the picture */
void host_present(const HostFrame *f)
{
    char path[512];
    if (g_hashes) {
        uint64_t h = 1469598103934665603ull;
        size_t k, n = (size_t)f->width * (size_t)f->height;
        for (k = 0; k < n; k++) h = (h ^ f->pixels[k]) * 1099511628211ull;
        fprintf(g_hashes, "%ld %016llx %llu\n", g_frame, (unsigned long long)h, (unsigned long long)g_cpu.icount);
    }
    if (g_out && (g_frame % g_every) == 0) {
        snprintf(path, sizeof path, "%s/f%05ld.png", g_out, g_frame);
        png_write_rgb(path, f->width, f->height, f->pixels);
    }
}

int main(int argc, char **argv)
{
    char err[512];
    long frames, i;
    const char *script = argc > 4 ? argv[4] : "";
    if (argc < 3) { fprintf(stderr, "usage: p2test GAMEDIR FRAMES [OUTDIR] [script]\n"); return 1; }
    g_dir = argv[1];
    frames = atol(argv[2]);
    g_out = argc > 3 ? argv[3] : NULL;
    if (getenv("P2_EVERY")) g_every = atoi(getenv("P2_EVERY"));
    if (getenv("P2_HASH")) g_hashes = fopen(getenv("P2_HASH"), "w");
    if (getenv("P2_WAV")) { g_wav = fopen(getenv("P2_WAV"), "wb"); if (g_wav) fseek(g_wav, 44, SEEK_SET); }
    if (getenv("P2_COV")) { g_cov = (uint8_t *)calloc(65536, 1); g_cov_cs = (uint16_t)(LOAD_SEG + 0x20); }
    if (!hal_init(err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    for (i = 0; i < frames; i++) {
        const char *p = script;
        g_frame = i;
        while (p && *p) {
            long fr; unsigned sc; int down;
            if (sscanf(p, "%ld:%x:%d", &fr, &sc, &down) == 3 && fr == i) hal_key((uint8_t)(sc & 0x7F), down, (sc & 0x100) != 0);
            p = strchr(p, ',');
            if (p) p++;
        }
        if (!hal_frame()) { fprintf(stderr, "program ended at frame %ld: %s\n", i, g_hal_exit_msg); break; }
    }
    wav_close();
    if (getenv("P2_VRAM")) vga_debug_dump(getenv("P2_VRAM"));
    if (g_cov) {
        FILE *cf = fopen(getenv("P2_COV"), "ab");
        if (cf) { long k; for (k = 0; k < 65536; k++) if (g_cov[k]) fprintf(cf, "%04lx\n", k); fclose(cf); }
    }
    fprintf(stderr, "ran %ld frames, %llu instructions, cs:ip %04x:%04x, %llu native blocks\n", i,
            (unsigned long long)g_cpu.icount, g_cpu.s[CS], g_cpu.ip, (unsigned long long)g_gen_blocks_run);
    if (g_hashes) fclose(g_hashes);
    return 0;
}
