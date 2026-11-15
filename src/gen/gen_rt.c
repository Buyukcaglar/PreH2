/* gen_rt.c - runs the recompiled blocks: a table from the code segment's offsets to the blocks, and
 * the hook through which cpu_run hands CS:IP over to them.  Code without a block (not found by the
 * analysis) and self-modified code run in the interpreter. */
#include "gen.h"
#include <string.h>

static GenFn g_table[65536];
uint64_t g_gen_blocks_run;

static int gen_hook(Cpu *c, uint64_t until)
{
    GenFn f;
    int ran = 0;
    if (c->s[CS] != GAME_CS || (f = g_table[c->ip]) == NULL) return 0;
    for (;;) {
        if (g_ip_hook[c->ip] && g_ip_hook_fn != NULL) {
            g_ip_hook_fn(c);                       /* it leaves the machine as it found it */
            if (c->stop) break;
        }
        if (!f(c)) break;
        ran = 1;
        g_gen_blocks_run++;
        if (c->irq_pending || c->stop || c->halted || c->icount >= until || c->s[CS] != GAME_CS) break;
        if ((f = g_table[c->ip]) == NULL) break;
    }
    return ran;
}

int gen_has_block(uint16_t ip)
{
    return g_table[ip] != NULL;
}

void gen_install(int on)
{
    unsigned i;
    memset(g_table, 0, sizeof g_table);
    for (i = 0; i < g_gen_block_count; i++) g_table[g_gen_blocks[i].ip] = g_gen_blocks[i].fn;
    g_native_hook = on ? gen_hook : NULL;
}
