/* Link the common frontend to a test backend, with no libx86emu headers/core. */
#include "private/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures, live, inits, destroys, fail_init, fail_alloc, huge;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
struct CPU
{
    struct X86Registers regs;
    uint32_t hidden, halted;
};
struct Fixture
{
    X86Context *ctx;
    uint32_t mode, calls, reject, handled;
};
void *pi_alloc(size_t bytes)
{
    void *p = fail_alloc ? NULL : calloc(1, bytes);
    if (p)
        ++live;
    return p;
}
void pi_free(void *p)
{
    if (p) {
        --live;
        free(p);
    }
}
static struct CPU *cpu(X86Context *ctx)
{
    return xe_backend_data(ctx);
}
uint32_t xe_backend_bytes(void)
{
    return huge ? UINT32_MAX : sizeof(struct CPU);
}
int xe_backend_init(X86Context *ctx)
{
    ++inits;
    CHECK((uintptr_t)cpu(ctx) % _Alignof(uint64_t) == 0);
    cpu(ctx)->regs.size = sizeof(struct X86Registers);
    return fail_init;
}
void xe_backend_destroy(X86Context *ctx)
{
    (void)ctx;
    ++destroys;
}
void xe_backend_stop(X86Context *ctx)
{
    cpu(ctx)->halted = 1;
}
void xe_backend_get(X86Context *ctx, struct X86Registers *r)
{
    *r = cpu(ctx)->regs;
}
void xe_backend_set(X86Context *ctx, const struct X86Registers *r, int reset)
{
    cpu(ctx)->regs = *r;
    if (reset)
        cpu(ctx)->hidden = 0;
}
void xe_backend_run(X86Context *ctx)
{
    struct Fixture *f = ctx->config.user;
    unsigned i;
    cpu(ctx)->halted = 0;
    if (f->mode == 1) {
        if (!xe_instruction(ctx))
            xe_interrupt(ctx, 0x60, 0);
    } else if (f->mode == 2) {
        if (!xe_instruction(ctx))
            xe_interrupt(ctx, 6, 1);
    } else if (f->mode == 3) {
        xe_stop(ctx, X86_STOP_LIMIT);
        xe_fault(ctx, 7);
    } else
        for (i = 0; i < 4 && !cpu(ctx)->halted; ++i) {
            uint32_t value = 0;
            if (xe_instruction(ctx))
                break;
            if (xe_access(ctx, 0x1000 + i, 1, X86_FETCH, &value))
                break;
            cpu(ctx)->regs.eax += value;
        }
}
static uint32_t X86_CALLBACK memory(void *user, uint32_t address, uint32_t width, uint32_t operation, uint32_t *value)
{
    struct Fixture *f     = user;
    struct X86Registers r = {.size = sizeof(r)};
    struct X86Limits l    = {sizeof(l), 1, 1, 0, NULL};
    struct X86Result out  = {.size = sizeof(out)};
    if (f->mode != 4)
        CHECK(address == 0x1000 + f->calls && width == 1 && operation == X86_FETCH);
    ++f->calls;
    CHECK(xe_get(f->ctx, &r) == X86_STOP_BUSY);
    CHECK(xe_set(f->ctx, &r) == X86_STOP_BUSY);
    CHECK(xe_delete(f->ctx) == X86_STOP_BUSY);
    CHECK(xe_run(f->ctx, &l, &out) == X86_STOP_BUSY);
    *value = 3;
    return f->reject;
}
static uint32_t X86_CALLBACK interrupt(void *user, uint32_t number, uint32_t fault, struct X86Registers *r)
{
    struct Fixture *f = user;
    CHECK((number == 0x60 && !fault) || (number == 6 && fault));
    r->eax = 0x1234;
    return f->handled;
}
static struct X86Result execute(struct Fixture *f, unsigned instructions, unsigned accesses, unsigned reason)
{
    struct X86Limits limits = {sizeof(limits), instructions, accesses, 0, NULL};
    struct X86Result result = {.size = sizeof(result)};
    f->calls                = 0;
    CHECK(xe_run(f->ctx, &limits, &result) == reason);
    CHECK(result.reason == reason);
    return result;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct Fixture f = {0}, g = {0};
    struct X86Config config = {
        .size = sizeof(config), .version = X86EMU_ABI_VERSION, .access = memory, .interrupt = interrupt, .user = &f};
    struct X86Registers r = {.size = sizeof(r)}, snapshot = {.size = sizeof(snapshot)};
    struct X86Result result;
    unsigned before;
    CHECK(xe_create(NULL) == NULL);
    fail_alloc = 1;
    CHECK(xe_create(&config) == NULL);
    CHECK(live == 0 && inits == 0);
    fail_alloc = 0;
    fail_init  = 1;
    CHECK(xe_create(&config) == NULL);
    CHECK(live == 0 && inits == 1 && destroys == 1);
    fail_init = 0;
    huge      = 1;
    CHECK(xe_context_bytes() == 0);
    CHECK(xe_create(&config) == NULL);
    CHECK(live == 0 && inits == 1);
    huge  = 0;
    f.ctx = xe_create(&config);
    CHECK(f.ctx != NULL);
    config.user = &g;
    g.ctx       = xe_create(&config);
    CHECK(g.ctx != NULL && live == 2);
    CHECK(xe_set(f.ctx, &r) == 0);
    result = execute(&f, 10, 10, X86_STOP_HALT);
    CHECK(result.instructions == 4 && result.accesses == 4 && f.calls == 4);
    CHECK(xe_get(f.ctx, &snapshot) == 0 && snapshot.eax == 12);
    CHECK(xe_get(g.ctx, &snapshot) == 0 && snapshot.eax == 0);
    result = execute(&f, 2, 10, X86_STOP_LIMIT);
    CHECK(result.instructions == 2 && result.accesses == 2 && f.calls == 2);
    result = execute(&f, 10, 1, X86_STOP_LIMIT);
    CHECK(result.instructions == 2 && result.accesses == 1 && f.calls == 1);
    f.reject = 1;
    result   = execute(&f, 10, 10, X86_STOP_ACCESS);
    CHECK(result.address == 0x1000 && result.operation == X86_FETCH && f.calls == 1);
    f.reject = 0;
    {
        volatile uint32_t cancel = 1;
        struct X86Limits limits  = {sizeof(limits), 10, 10, 0, &cancel};
        result                   = (struct X86Result){.size = sizeof(result)};
        CHECK(xe_run(f.ctx, &limits, &result) == X86_STOP_CANCEL);
        CHECK(result.instructions == 0 && result.accesses == 0);
    }
    f.mode             = 1;
    f.handled          = 1;
    cpu(f.ctx)->hidden = 77;
    result             = execute(&f, 10, 10, X86_STOP_HALT);
    CHECK(cpu(f.ctx)->hidden == 77 && cpu(f.ctx)->regs.eax == 0x1234);
    CHECK(xe_set(f.ctx, &r) == 0 && cpu(f.ctx)->hidden == 0);
    f.mode    = 2;
    f.handled = 0;
    result    = execute(&f, 10, 10, X86_STOP_FAULT);
    CHECK(result.interrupt == 6);
    f.mode = 3;
    result = execute(&f, 10, 10, X86_STOP_LIMIT);
    CHECK(result.interrupt == 7);
    before = destroys;
    CHECK(xe_delete(f.ctx) == 0 && xe_delete(g.ctx) == 0);
    CHECK(live == 0 && destroys == before + 2);
    {
        uint8_t code[]                 = {0x78, 0x56, 0x34, 0x12};
        struct X86CodeRegion regions[] = {{0x1000, 4, code}, {0x2000, 4, code}};
        struct X86Config mapped        = {.size         = sizeof(mapped),
                                          .version      = X86EMU_ABI_VERSION,
                                          .access       = memory,
                                          .interrupt    = interrupt,
                                          .user         = &f,
                                          .region_count = 2,
                                          .regions      = regions};
        uint32_t value                 = 0;
        f.mode                         = 4;
        f.reject                       = 0;
        f.ctx                          = xe_create(&mapped);
        CHECK(f.ctx != NULL);
        CHECK(f.ctx->region_count == 2);
        regions[0].address     = 0; /* descriptors are copied */
        f.ctx->limits.accesses = 10;
        f.ctx->active          = 1;
        f.calls                = 0;
        CHECK(xe_access(f.ctx, 0x1000, 4, X86_FETCH, &value) == 0);
        CHECK(value == 0x12345678 && f.calls == 0 && f.ctx->result.accesses == 1);
        code[0] = 0xef;
        CHECK(xe_access(f.ctx, 0x1000, 1, X86_FETCH, &value) == 0 && value == 0xef);
        CHECK(xe_access(f.ctx, 0x1000, 1, X86_READ, &value) == 0 && f.calls == 1);
        f.reject = 1;
        CHECK(xe_access(f.ctx, 0x1002, 4, X86_FETCH, &value) == 1);
        CHECK(f.ctx->result.reason == X86_STOP_ACCESS);
        f.ctx->active = 0;
        CHECK(xe_delete(f.ctx) == 0 && live == 0);
        mapped.region_count = 9;
        CHECK(xe_create(&mapped) == NULL);
        mapped.region_count = 1;
        mapped.regions      = NULL;
        CHECK(xe_create(&mapped) == NULL);
        mapped.regions  = regions;
        regions[0].data = NULL;
        CHECK(xe_create(&mapped) == NULL);
        regions[0].data  = code;
        regions[0].bytes = 0;
        CHECK(xe_create(&mapped) == NULL);
        regions[0].bytes   = 4;
        regions[0].address = 0xfffffffe;
        CHECK(xe_create(&mapped) == NULL);
        regions[0].address  = 0x2001;
        mapped.region_count = 2;
        CHECK(xe_create(&mapped) == NULL);
        regions[0].address = 0x1000;
        --mapped.size;
        CHECK(xe_create(&mapped) == NULL);
        ++mapped.size;
        mapped.version = X86EMU_ABI_VERSION + 1;
        CHECK(xe_create(&mapped) == NULL);
        mapped.version       = X86EMU_ABI_VERSION;
        mapped.bulk_elements = X86_SPAN_MAX + 1;
        CHECK(xe_create(&mapped) == NULL);
        mapped.bulk_elements = 0;
        mapped.data_regions  = 4;
        CHECK(xe_create(&mapped) == NULL);
        CHECK(live == 0);
    }
    printf("BACKEND CONTRACT RESULT checks=%u failures=%u live=%u\n", checks, failures, live);
    return failures ? 20 : 0;
}
