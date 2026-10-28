#ifndef PCIINIT_LIBX86EMU_BACKEND_H
#define PCIINIT_LIBX86EMU_BACKEND_H
#include "private/backend.h"
#include "x86emu_int.h"
struct LibX86State
{
    x86emu_t cpu;
    uint64_t msr[X86EMU_MSRS];
    unsigned char msr_perm[X86EMU_MSRS];
    unsigned step_remaining;
};

/* Shared interpreter adapter. CPU state is retained in full across fallback;
 * no public-register snapshot round trip loses hidden machine state. */
int xe_lib_init(X86Context *ctx);
void xe_lib_destroy(X86Context *ctx);
void xe_lib_get(X86Context *ctx, struct X86Registers *r);
void xe_lib_set(X86Context *ctx, const struct X86Registers *r, int reset);
void xe_lib_run(X86Context *ctx);
void xe_lib_stop(X86Context *ctx);
int xe_lib_step(X86Context *ctx);
void x86emu_handle_pending(x86emu_t *cpu);
#endif
