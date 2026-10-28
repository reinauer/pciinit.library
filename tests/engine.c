#include "private/engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
struct fixture
{
    unsigned char ram[0x20000];
    unsigned io, ints;
    X86Context *ctx;
};
static uint32_t X86_CALLBACK memory(void *user, uint32_t address, uint32_t width, uint32_t op, uint32_t *value)
{
    struct fixture *f = user;
    uint32_t i, v = 0;
    if (op == X86_IN) {
        *value = 0x78563412;
        ++f->io;
        return 0;
    }
    if (op == X86_OUT) {
        ++f->io;
        return 0;
    }
    if (address > sizeof(f->ram) || width > sizeof(f->ram) - address)
        return 1;
    if (op == X86_WRITE)
        for (i = 0; i < width; ++i)
            f->ram[address + i] = *value >> (i * 8);
    else {
        for (i = 0; i < width; ++i)
            v |= (uint32_t)f->ram[address + i] << (8 * i);
        *value = v;
    }
    return 0;
}
static uint32_t X86_CALLBACK intr(void *user, uint32_t num, uint32_t fault, struct X86Registers *r)
{
    struct fixture *f = user;
    if (fault)
        return 2;
    ++f->ints;
    if (num == 0x60) {
        struct X86Registers tmp = {.size = sizeof(tmp)};
        CHECK(xe_get(f->ctx, &tmp) == X86_STOP_BUSY);
        CHECK(xe_delete(f->ctx) == X86_STOP_BUSY);
        r->eax = 0x12345678;
        return 1;
    }
    return 0;
}
static struct fixture *new_fixture(void)
{
    struct fixture *f  = calloc(1, sizeof(*f));
    struct X86Config c = {
        .size = sizeof(c), .version = X86EMU_ABI_VERSION, .access = memory, .interrupt = intr, .user = f};
#ifdef ENGINE_MAPPED
    struct X86CodeRegion region = {0, sizeof(f->ram), f->ram};
    c.region_count              = 1;
    c.regions                   = &region;
    if (!f || !(f->ctx = xe_create(&c)))
#else
    if (!f || !(f->ctx = xe_create(&c)))
#endif
    {
        puts("FAIL allocation");
        exit(20);
    }
    return f;
}
static struct X86Registers run(struct fixture *f, const unsigned char *code, size_t bytes, unsigned expected,
                               unsigned budget)
{
    struct X86Registers r   = {.size = sizeof(r), .esp = 0xff00, .eip = 0x1000, .eflags = 2};
    struct X86Limits limits = {sizeof(limits), budget, 10000, 1000, NULL};
    struct X86Result result = {.size = sizeof(result)};
    memcpy(f->ram + 0x1000, code, bytes);
    CHECK(xe_set(f->ctx, &r) == 0);
    CHECK(xe_run(f->ctx, &limits, &result) == expected);
    CHECK(result.reason == expected);
    CHECK(xe_get(f->ctx, &r) == 0);
    return r;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct fixture *f = new_fixture(), *g = new_fixture();
    struct X86Registers r;
    static const unsigned char regs[]      = {0x66, 0xb8, 0x78, 0x56, 0x34, 0x12, 0xb4, 0xab, 0xb0, 0xcd, 0xf4};
    static const unsigned char unaligned[] = {0x66, 0xb8, 0x78, 0x56, 0x34, 0x12, 0x66, 0xa3, 0x01,
                                              0x20, 0x66, 0x31, 0xc0, 0x66, 0xa1, 0x01, 0x20, 0xf4};
    static const unsigned char bsf[]       = {0x66, 0xb8, 0, 0, 0x10, 0, 0x66, 0x0f, 0xbc, 0xc0, 0xf4};
    static const unsigned char bsr[]       = {0x66, 0xb8, 0, 0, 0x10, 0, 0x66, 0x0f, 0xbd, 0xc0, 0xf4};
    static const unsigned char jmp[]       = {0x66, 0xe9, 0x02, 0, 0, 0, 0x0f, 0x0b, 0xb8, 0xef, 0xbe, 0xf4};
    /* 67 controls addressing; 66 independently controls destination width. */
    static const unsigned char lea16[]    = {0x66, 0xb8, 0,    0,    0xcd, 0xab, 0x66, 0xbb, 0x78,
                                             0x56, 0x34, 0x12, 0x67, 0x8d, 0x43, 0x01, 0xf4};
    static const unsigned char lea32[]    = {0xbb, 0x34, 0x12, 0x66, 0x8d, 0x47, 0x01, 0xf4};
    static const unsigned char call[]     = {0xe8, 0x01, 0, 0xf4, 0xb8, 0x12, 0x34, 0xc3};
    static const unsigned char irq[]      = {0xcd, 0x61, 0xf4};
    static const unsigned char hook[]     = {0xcd, 0x60, 0xf4};
    static const unsigned char loop[]     = {0xeb, 0xfe};
    static const unsigned char bad[]      = {0x0f, 0x0b};
    static const unsigned char outside[]  = {0x67, 0xa1, 0, 0, 0x10, 0, 0xf4};
    static const unsigned char rep[]      = {0x66, 0xb9, 0xff, 0xff, 0xff, 0xff, 0x67, 0xf3, 0xaa, 0xf4};
    static const unsigned char farcall[]  = {0x9a, 0x00, 0x00, 0x00, 0x03, 0xf4};
    static const unsigned char call32[]   = {0x66, 0xe8, 1, 0, 0, 0, 0xf4, 0xb8, 0x34, 0x12, 0x66, 0xc3};
    static const unsigned char far32[]    = {0x66, 0x9a, 0, 0, 0, 0, 0, 3, 0xf4};
    static const unsigned char farjmp32[] = {0x66, 0xea, 0, 0x30, 0, 0, 0, 0};
    unsigned char prefixes[32];
    r = run(f, regs, sizeof(regs), 0, 50);
    CHECK(r.eax == 0x1234abcd);
    r = run(f, unaligned, sizeof(unaligned), 0, 50);
    CHECK(r.eax == 0x12345678);
    CHECK(f->ram[0x2001] == 0x78);
    r = run(f, bsf, sizeof(bsf), 0, 50);
    CHECK(r.eax == 20);
    r = run(f, bsr, sizeof(bsr), 0, 50);
    CHECK(r.eax == 20);
    r = run(f, jmp, sizeof(jmp), 0, 50);
    CHECK((r.eax & 65535) == 0xbeef);
    r = run(f, lea16, sizeof(lea16), 0, 50);
    CHECK(r.eax == 0xabcd5679);
    r = run(f, lea32, sizeof(lea32), 0, 50);
    CHECK(r.eax == 0x1235);
    r = run(f, call, sizeof(call), 0, 50);
    CHECK(r.eax == 0x3412);
    CHECK(r.esp == 0xff00);
    f->ram[0x184]  = 0x00;
    f->ram[0x185]  = 0x30;
    f->ram[0x3000] = 0xb8;
    f->ram[0x3001] = 0x42;
    f->ram[0x3002] = 0;
    f->ram[0x3003] = 0xcf;
    r              = run(f, irq, sizeof(irq), 0, 50);
    CHECK(r.eax == 0x42);
    CHECK(r.esp == 0xff00);
    r = run(f, hook, sizeof(hook), 0, 50);
    CHECK(r.eax == 0x12345678);
    f->ram[0x3000] = 0xb8;
    f->ram[0x3001] = 0x55;
    f->ram[0x3002] = 0x66;
    f->ram[0x3003] = 0xcb;
    r              = run(f, farcall, sizeof(farcall), 0, 50);
    CHECK(r.eax == 0x6655);
    CHECK(r.esp == 0xff00);
    r = run(f, call32, sizeof(call32), 0, 50);
    CHECK(r.eax == 0x1234);
    CHECK(r.esp == 0xff00);
    f->ram[0x3003] = 0x66;
    f->ram[0x3004] = 0xcb;
    r              = run(f, far32, sizeof(far32), 0, 50);
    CHECK(r.eax == 0x6655);
    CHECK(r.esp == 0xff00);
    f->ram[0x3003] = 0xf4;
    r              = run(f, farjmp32, sizeof(farjmp32), 0, 50);
    CHECK(r.eax == 0x6655);
    run(f, loop, sizeof(loop), X86_STOP_LIMIT, 10);
    run(f, bad, sizeof(bad), X86_STOP_FAULT, 50);
    {
        static const unsigned char aam_zero[] = {0xb8, 0x9f, 0x12, 0xd4, 0, 0xf4};
        r                                     = run(f, aam_zero, sizeof(aam_zero), X86_STOP_FAULT, 50);
        CHECK(r.eax == 0x129f);
    }
    run(f, outside, sizeof(outside), X86_STOP_ACCESS, 50);
    run(f, rep, sizeof(rep), X86_STOP_LIMIT, 50);
    memset(prefixes, 0xf3, sizeof(prefixes));
    /* An excessive prefix stream must terminate, even with no final opcode. */
    run(f, prefixes, sizeof(prefixes), X86_STOP_FAULT, 50);
    r = run(g, regs, sizeof(regs), 0, 50);
    CHECK(r.eax == 0x1234abcd);
    CHECK(g->ints == 0);
    {
        volatile uint32_t cancel  = 1;
        struct X86Limits limits   = {sizeof(limits), 0xffffffffU, 0xffffffffU, 1, &cancel};
        struct X86Result result   = {.size = sizeof(result)};
        struct X86Registers entry = {.size = sizeof(entry), .eip = 0x1000};
        memcpy(f->ram + 0x1000, loop, sizeof(loop));
        CHECK(xe_set(f->ctx, &entry) == 0);
        CHECK(xe_run(f->ctx, &limits, &result) == X86_STOP_CANCEL);
        CHECK(result.instructions == 0 && result.accesses == 0);
        cancel = 0;
        CHECK(xe_set(f->ctx, &entry) == 0);
        CHECK(xe_run(f->ctx, &limits, &result) == X86_STOP_TIMEOUT);
        CHECK(result.instructions > 0);
        limits.instructions = 0;
        CHECK(xe_run(f->ctx, &limits, &result) == X86_STOP_BADARG);
    }
    CHECK(xe_delete(f->ctx) == 0);
    CHECK(xe_delete(g->ctx) == 0);
    free(f);
    free(g);
    printf("ENGINE RESULT checks=%u failures=%u context_bytes=%lu\n", checks, failures,
           (unsigned long)xe_context_bytes());
    return failures ? 20 : 0;
}
