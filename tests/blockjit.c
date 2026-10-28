/* Differential native-code tests plus cache/budget/callback boundaries. */
#include "private/backend.h"
#include "private/m68kjit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct Fixture
{
    uint8_t ram[0x20000];
    X86Context *ctx;
    unsigned writes, io, io_hash, io_mode;
    volatile uint32_t cancel;
};
static unsigned cases, failures;
static uint32_t random_state = 0x27182818;
static uint32_t random_value(void)
{
    random_state = random_state * 1664525 + 1013904223;
    return random_state;
}
static uint32_t X86_CALLBACK access_memory(void *user, uint32_t address, uint32_t width, uint32_t op, uint32_t *v)
{
    struct Fixture *f = user;
    unsigned i;
    if (op == X86_IN || op == X86_OUT) {
        /* A callback can modify code that is already in the cache. */
        f->ram[0x1001] ^= 0x55;
        f->ram[0x1010] ^= 0x11;
        ++f->io;
        f->io_hash = f->io_hash * 31 + address * 17 + width * 7 + op;
        if (op == X86_OUT)
            f->io_hash = f->io_hash * 31 + *v;
        else
            *v = 0xdeadbe79;
        if (f->io_mode == 2)
            f->cancel = 1;
        return f->io_mode == 1;
    }
    if (address > sizeof(f->ram) || width > sizeof(f->ram) - address)
        return 1;
    if (op == X86_WRITE) {
        ++f->writes;
        for (i = 0; i < width; ++i)
            f->ram[address + i] = *v >> (8 * i);
    } else {
        *v = 0;
        for (i = 0; i < width; ++i)
            *v |= (uint32_t)f->ram[address + i] << (8 * i);
    }
    return 0;
}
static uint32_t X86_CALLBACK interrupt(void *user, uint32_t n, uint32_t fault, struct X86Registers *r)
{
    struct Fixture *f = user;
    if (fault || n != 0x60)
        return 2;
    f->ram[0x1001] ^= 0xaa;
    r->eax ^= 0x6789;
    return 1;
}
static struct Fixture *create(int mapped)
{
    struct Fixture *f           = calloc(1, sizeof(*f));
    struct X86CodeRegion region = {0, sizeof(f->ram), f->ram};
    struct X86Config c          = {.size         = sizeof(c),
                                   .version      = X86EMU_ABI_VERSION,
                                   .access       = access_memory,
                                   .interrupt    = interrupt,
                                   .user         = f,
                                   .region_count = mapped ? 1 : 0,
                                   .regions      = &region};
    if (!f || !(f->ctx = xe_create(&c)))
        exit(20);
    return f;
}
static void compare(struct Fixture *reference, struct Fixture *jit, const uint8_t *code, unsigned length,
                    unsigned instructions, unsigned accesses)
{
    struct X86Registers regs[2];
    struct X86Result results[2];
    struct X86Limits limits = {sizeof(limits), instructions, accesses, 0, NULL};
    struct Fixture *f[]     = {reference, jit};
    unsigned i;
    struct X86Registers initial = {.size   = sizeof(initial),
                                   .eip    = 0x1000,
                                   .esp    = 0xff00,
                                   .eax    = random_value(),
                                   .ebx    = random_value(),
                                   .ecx    = random_value(),
                                   .edx    = random_value(),
                                   .esi    = random_value(),
                                   .edi    = random_value(),
                                   .ebp    = random_value(),
                                   .eflags = 2 | (random_value() & 0xcd5)};
    for (i = 0; i < 2; ++i) {
        f[i]->writes = f[i]->io = f[i]->io_hash = f[i]->cancel = 0;
        limits.cancel                                          = &f[i]->cancel;
        memset(f[i]->ram, 0, sizeof(f[i]->ram));
        memcpy(f[i]->ram + 0x1000, code, length);
        if (xe_set(f[i]->ctx, &initial))
            exit(20);
        results[i] = (struct X86Result){.size = sizeof(results[i])};
        xe_run(f[i]->ctx, &limits, &results[i]);
        regs[i] = (struct X86Registers){.size = sizeof(regs[i])};
        xe_get(f[i]->ctx, &regs[i]);
    }
    ++cases;
    if (memcmp(regs, regs + 1, sizeof(regs[0])) || memcmp(results, results + 1, sizeof(results[0])) ||
        memcmp(reference->ram, jit->ram, sizeof(jit->ram)) || reference->io != jit->io ||
        reference->io_hash != jit->io_hash || reference->writes != jit->writes) {
        printf(
            "FAIL case=%u op=%02x budget=%u/%u eax=%lx/%lx flags=%lx/%lx ip=%lx/%lx stop=%lu/%lu insn=%lu/%lu "
            "access=%lu/%lu\n",
            cases, code[0], instructions, accesses, (unsigned long)regs[0].eax, (unsigned long)regs[1].eax,
            (unsigned long)regs[0].eflags, (unsigned long)regs[1].eflags, (unsigned long)regs[0].eip,
            (unsigned long)regs[1].eip, (unsigned long)results[0].reason, (unsigned long)results[1].reason,
            (unsigned long)results[0].instructions, (unsigned long)results[1].instructions,
            (unsigned long)results[0].accesses, (unsigned long)results[1].accesses);
        if (++failures == 10)
            exit(20);
    }
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct Fixture *reference = create(0), *jit = create(1);
    struct X86JitStats stats;
    unsigned size, op, dst, src, trial, n, i, j;
    uint8_t code[256];
    for (size = 0; size < 3; ++size)
        for (op = 0; op < 8; ++op)
            for (dst = 0; dst < 8; ++dst)
                for (src = 0; src < 8; ++src) {
                    n = 0;
                    for (trial = 0; trial < 4; ++trial) {
                        if (size == 2)
                            code[n++] = 0x66;
                        code[n++] = op * 8 + (size != 0);
                        code[n++] = 0xc0 + (src << 3) + dst;
                    }
                    code[n++] = 0xf4;
                    compare(reference, jit, code, n, 100, 100);
                }
    /* Register exchanges include overlapping AL/AH and identical operands. */
    for (size = 0; size < 3; ++size)
        for (dst = 0; dst < 8; ++dst)
            for (src = 0; src < 8; ++src) {
                n = 0;
                if (size == 2)
                    code[n++] = 0x66;
                code[n++] = 0x86 + (size != 0);
                code[n++] = 0xc0 + (src << 3) + dst;
                code[n++] = 0xf4;
                compare(reference, jit, code, n, 100, 100);
            }
    /* Every I/O encoding, width, budget edge, callback fault and cancel.
     * The callback mutates both earlier and following mapped code. */
    for (trial = 0; trial < 3; ++trial) {
        reference->io_mode = jit->io_mode = trial;
        for (size = 0; size < 2; ++size)
            for (op = 0xe4; op <= 0xef; ++op) {
                if (op >= 0xe8 && op < 0xec)
                    continue;
                n         = 0;
                code[n++] = 0x40;
                code[n++] = 0x42;
                if (size)
                    code[n++] = 0x66;
                code[n++] = op;
                if (op < 0xe8)
                    code[n++] = 0x81;
                while (n < 0x10)
                    code[n++] = 0x90;
                code[n++] = 0xb8;
                code[n++] = 0x34;
                code[n++] = 0x12;
                code[n++] = 0xf4;
                for (i = 1; i < 20; ++i)
                    for (j = 1; j < 25; ++j)
                        compare(reference, jit, code, n, i, j);
            }
    }
    reference->io_mode = jit->io_mode = 0;
    /* Immediate/sign extension, unary, exchange, shifts and branch blocks. */
    for (trial = 0; trial < 256; ++trial) {
        n = 0;
        for (i = 0; i < 16; ++i) {
            unsigned wide  = random_value() & 1;
            uint32_t value = random_value();
            if (wide)
                code[n++] = 0x66;
            if (i % 4 == 0) {
                code[n++] = 0xb8 + (random_value() & 7);
                for (j = 0; j < (wide ? 4U : 2U); ++j)
                    code[n++] = value >> (j * 8);
            } else if (i % 4 == 1) {
                code[n++] = 0x83;
                code[n++] = 0xc0 + (random_value() & 63);
                code[n++] = value;
            } else if (i % 4 == 2) {
                code[n++] = 0xc1;
                code[n++] = 0xc0 + (random_value() & 63);
                code[n++] = value & 31;
            } else
                code[n++] = 0x40 + (random_value() & 15);
        }
        code[n++] = 0xf4;
        compare(reference, jit, code, n, 100, 200);
    }
    for (op = 0x70; op <= 0x7f; ++op)
        for (trial = 0; trial < 16; ++trial) {
            uint8_t branch[] = {0x39, 0xd8, op, 3, 0xb8, 0x34, 0x12, 0xf4};
            compare(reference, jit, branch, sizeof(branch), 100, 100);
        }
    {
        const uint8_t mixed[] = {0xb8, 0x34, 0x12, 0xb9, 5,    0,    0x40, 0x50, 0x5b,
                                 0xe2, 0xfa, 0xe6, 0x80, 0xcd, 0x60, 0xeb, 0xef};
        for (i = 1; i < 24; ++i)
            for (j = 1; j < 40; ++j)
                compare(reference, jit, mixed, sizeof(mixed), i, j);
    }
    /* Cached semantic handlers retain prefix order, operand fetching,
     * fault restart state and exact access-budget edges for every opcode. */
    {
        static const uint8_t prefixes[] = {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf0, 0xf2, 0xf3};
        for (op = 0; op < 256; ++op)
            for (src = 0; src < sizeof(prefixes); ++src) {
                memset(code, 0, sizeof(code));
                n = 0;
                if (prefixes[src])
                    code[n++] = prefixes[src];
                code[n++] = op;
                code[n++] = 0xc0;
                code[n++] = 0x03;
                code[n++] = 0;
                code[n++] = 0;
                code[n++] = 0xf4;
                for (i = 1; i <= 5; i += 2)
                    compare(reference, jit, code, n, 2, i);
            }
    }
    xe_jit_stats(jit->ctx, &stats);
    if (!stats.instructions || !stats.blocks || !stats.invalidated || !stats.fallback)
        ++failures;
    printf("BLOCKJIT RESULT cases=%u failures=%u native=%lu blocks=%lu compiled=%lu invalidated=%lu fallback=%lu\n",
           cases, failures, (unsigned long)stats.instructions, (unsigned long)stats.blocks,
           (unsigned long)stats.compiled, (unsigned long)stats.invalidated, (unsigned long)stats.fallback);
    if (xe_delete(reference->ctx) || xe_delete(jit->ctx))
        ++failures;
    free(reference);
    free(jit);
    return failures ? 20 : 0;
}
