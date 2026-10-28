#ifndef PCIINIT_M68KJIT_H
#define PCIINIT_M68KJIT_H
#include "private/engine.h"
/* Private validation counters, cumulative over the context lifetime. */
struct X86JitStats
{
    uint32_t compiled, blocks, instructions, fallback, invalidated;
};
void xe_jit_stats(X86Context *ctx, struct X86JitStats *stats);
#endif
