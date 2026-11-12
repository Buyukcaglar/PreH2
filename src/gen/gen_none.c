/* gen_none.c - an empty block table, used by build.cmd when src/gen/gen_all.c has not been
 * generated (python tools/p2rec.py, from your own copy of the game).  The game then runs in the
 * interpreter, which is slower but gives the same result. */
#include "gen.h"
#include <stddef.h>

const GenBlock g_gen_blocks[1] = {{0u, NULL}};
const unsigned g_gen_block_count = 0u;
