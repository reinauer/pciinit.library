/* Benchmark the public engine contract, including common callback policy. */
#include "bench.h"
#include "private/engine.h"
#include <stdlib.h>
static X86Context *context;
static uint32_t access_memory(void *user, uint32_t address, uint32_t width, uint32_t operation, uint32_t *value)
{
    (void)user;
    if (operation == X86_IN)
        *value = read_port(address);
    else if (operation == X86_OUT)
        write_port(address, *value);
    else if (operation == X86_WRITE)
        write_mem(address, width, *value);
    else
        *value = read_mem(address, width);
    return 0;
}
void engine_reset(void)
{
    struct X86Config config = {.size = sizeof(config), .version = X86EMU_ABI_VERSION, .access = access_memory};
    struct X86Registers r   = {.size = sizeof(r), .eip = 0x1000, .esp = 0xf000, .eflags = 2};
#ifdef WRAPPER_MAPPED
    struct X86CodeRegion region = {0, sizeof(ram), ram};
    config.region_count         = 1;
    config.regions              = &region;
    if (!context)
        context = xe_create(&config);
#else
    if (!context)
        context = xe_create(&config);
#endif
    if (!context || xe_set(context, &r))
        exit(26);
}
struct result engine_run(void)
{
    struct X86Limits limits = {sizeof(limits), 100000, 1000000,
#ifdef WRAPPER_TIMED
                               0xffffffffU,
#else
                               0,
#endif
                               NULL};
    struct X86Result outcome = {.size = sizeof(outcome)};
    struct X86Registers r    = {.size = sizeof(r)};
    if (xe_run(context, &limits, &outcome) || xe_get(context, &r))
        exit(27);
    return (struct result){r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip, r.eflags};
}
unsigned engine_bytes(void)
{
    return xe_context_bytes();
}
void engine_seed(const uint32_t r[8], uint32_t flags)
{
    struct X86Registers regs = {.size   = sizeof(regs),
                                .eax    = r[0],
                                .ecx    = r[1],
                                .edx    = r[2],
                                .ebx    = r[3],
                                .esp    = r[4],
                                .ebp    = r[5],
                                .esi    = r[6],
                                .edi    = r[7],
                                .eflags = flags,
                                .eip    = 0x1000};
    if (xe_set(context, &regs))
        exit(28);
}
