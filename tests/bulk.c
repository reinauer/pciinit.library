/* Compare optional REP transfers with scalar execution and stop states. */
#include "private/engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture
{
    uint8_t ram[0x21000];
    uint32_t fault, partial, calls, operations, cancel_after, largest;
    volatile uint32_t cancel;
};
static unsigned cases, failures, bulk_calls, span_calls, bulk_largest, span_largest;
static int valid(struct fixture *f, uint32_t a, uint32_t w)
{
    return a < sizeof(f->ram) && w <= sizeof(f->ram) - a && !(a <= f->fault && f->fault - a < w);
}
static uint32_t X86_CALLBACK memory(void *user, uint32_t a, uint32_t w, uint32_t op, uint32_t *v)
{
    struct fixture *f = user;
    unsigned i;
    ++f->operations;
    if (f->cancel_after && f->operations >= f->cancel_after)
        f->cancel = 1;
    if (!valid(f, a, w))
        return 1;
    if (op == X86_WRITE)
        for (i = 0; i < w; ++i)
            f->ram[a + i] = *v >> (8 * i);
    else {
        *v = 0;
        for (i = 0; i < w; ++i)
            *v |= (uint32_t)f->ram[a + i] << (8 * i);
    }
    return 0;
}
static uint32_t X86_CALLBACK bulk(void *user, const struct X86BulkRequest *b)
{
    struct fixture *f = user;
    uint32_t n = b->count, i, src = b->source, dst = b->destination;
    ++f->calls;
    if (n > f->largest)
        f->largest = n;
    if (n > f->partial)
        n = f->partial;
    for (i = 0; i < n; ++i, src += b->stride, dst += b->stride) {
        uint32_t value = b->value;
        if (!valid(f, dst, b->width) || (b->operation == X86_BULK_COPY && !valid(f, src, b->width)))
            break;
        if (b->operation == X86_BULK_COPY)
            memory(f, src, b->width, X86_READ, &value);
        memory(f, dst, b->width, X86_WRITE, &value);
    }
    return i;
}
static uint32_t X86_CALLBACK interrupt(void *u, uint32_t n, uint32_t fault, struct X86Registers *r)
{
    (void)u;
    (void)n;
    (void)fault;
    (void)r;
    return 2;
}
static void test(unsigned copy, unsigned width, unsigned flags, unsigned count, unsigned budget, unsigned variant)
{
    struct fixture *f = calloc(1, sizeof(*f)), *g = calloc(1, sizeof(*g)), *h = calloc(1, sizeof(*h));
    struct X86CodeRegion rf = {0, sizeof(f->ram), f->ram}, rg = {0, sizeof(g->ram), g->ram},
                         rh = {0, sizeof(h->ram), h->ram};
    /* a: scalar only; b: bulk with the default X86_BULK_MAX requests;
     * e: bulk with whole-segment requests. */
    struct X86Config a = {.size         = sizeof(a),
                          .version      = X86EMU_ABI_VERSION,
                          .access       = memory,
                          .interrupt    = interrupt,
                          .user         = f,
                          .region_count = 1,
                          .regions      = &rf};
    struct X86Config b = a, e = a;
    struct X86Registers r = {.size   = sizeof(r),
                             .eip    = 0x1000,
                             .esp    = 0x8000,
                             .eax    = 0x12345678,
                             .ecx    = count,
                             .esi    = 0x4000,
                             .edi    = 0x6000,
                             .eflags = 2},
                        s;
    struct X86Limits lim = {sizeof(lim), 10, budget, 0, &f->cancel};
    struct X86Result x = {.size = sizeof(x)}, y = {.size = sizeof(y)}, z = {.size = sizeof(z)};
    X86Context *ca, *cb, *ce;
    unsigned i, len = 0;
    uint8_t code[12];
    struct X86Registers t;
    if (!f || !g || !h)
        exit(20);
    for (i = 0; i < sizeof(f->ram); ++i)
        f->ram[i] = (i * 13 + (i >> 8)) & 255;
    f->fault = g->fault = UINT32_MAX;
    g->partial          = variant == 1 ? 3 : 256;
    if (flags & 1)
        code[len++] = 0x67;
    if (width == 4)
        code[len++] = 0x66;
    if (flags & 2)
        r.eflags |= 0x400;
    if (flags & 4) {
        code[len++]       = 0x64;
        r.segment[X86_FS] = 0x100;
    }
    if (!(flags & 1)) {
        r.esi |= 0x12340000;
        r.edi |= 0x56780000;
        r.ecx |= 0xabcd0000;
    }
    if (variant == 2) {
        r.edi = (r.edi & 0xffff0000) | 0xfffe;
        r.esi = (r.esi & 0xffff0000) | 0xffff;
    }
    if (variant == 3)
        f->fault = g->fault = 0x6004;
    if (variant == 4)
        f->cancel_after = g->cancel_after = 13;
    if (variant == 5)
        r.edi = r.esi + 1; /* overlapping REP is not memmove */
    if (variant == 6)
        g->partial = 0; /* callback declines every operation */
    if (variant == 7) {
        r.edi      = 0x1000;
        g->partial = 2;
    } /* writes current/next code */
    code[len++] = (flags & 4) ? 0xf2 : 0xf3;
    code[len++] = (copy ? 0xa4 : 0xaa) + (width != 1);
    code[len++] = 0xf4;
    memcpy(f->ram + 0x1000, code, len);
    memcpy(g->ram, f->ram, sizeof(f->ram));
    s = r;
    memcpy(h->ram, f->ram, sizeof(f->ram));
    t               = r;
    h->fault        = g->fault;
    h->partial      = g->partial;
    h->cancel_after = g->cancel_after;
    b.user          = g;
    b.regions       = &rg;
    b.bulk          = bulk;
    e.user          = h;
    e.regions       = &rh;
    e.bulk          = bulk;
    e.bulk_elements = X86_SPAN_MAX;
    ca              = xe_create(&a);
    cb              = xe_create(&b);
    ce              = xe_create(&e);
    if (!ca || !cb || !ce)
        exit(20);
    xe_set(ca, &r);
    xe_set(cb, &s);
    xe_set(ce, &t);
    xe_run(ca, &lim, &x);
    lim.cancel = &g->cancel;
    xe_run(cb, &lim, &y);
    lim.cancel = &h->cancel;
    xe_run(ce, &lim, &z);
    xe_get(ca, &r);
    xe_get(cb, &s);
    xe_get(ce, &t);
    ++cases;
    bulk_calls += g->calls;
    span_calls += h->calls;
    if (g->largest > bulk_largest)
        bulk_largest = g->largest;
    if (h->largest > span_largest)
        span_largest = h->largest;
    /* The engine polls once per request: a cancel raised inside a request
     * is observed afterwards, so only its stop reason is compared. */
    if (variant == 4
            ? y.reason != x.reason
            : (memcmp(&r, &s, sizeof(r)) || memcmp(&x, &y, sizeof(x)) || memcmp(f->ram, g->ram, sizeof(f->ram)))) {
        ++failures;
        if (failures < 12)
            printf(
                "FAIL copy=%u width=%u flags=%u count=%u budget=%u variant=%u stop=%lu/%lu access=%lu/%lu di=%lx/%lx\n",
                copy, width, flags, count, budget, variant, (unsigned long)x.reason, (unsigned long)y.reason,
                (unsigned long)x.accesses, (unsigned long)y.accesses, (unsigned long)r.edi, (unsigned long)s.edi);
    }
    if (variant == 4
            ? z.reason != x.reason
            : (memcmp(&r, &t, sizeof(r)) || memcmp(&x, &z, sizeof(x)) || memcmp(f->ram, h->ram, sizeof(f->ram)))) {
        ++failures;
        if (failures < 12)
            printf(
                "FAIL span copy=%u width=%u flags=%u count=%u budget=%u variant=%u stop=%lu/%lu access=%lu/%lu "
                "di=%lx/%lx\n",
                copy, width, flags, count, budget, variant, (unsigned long)x.reason, (unsigned long)z.reason,
                (unsigned long)x.accesses, (unsigned long)z.accesses, (unsigned long)r.edi, (unsigned long)t.edi);
    }
    xe_delete(ca);
    xe_delete(cb);
    xe_delete(ce);
    free(f);
    free(g);
    free(h);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    static const unsigned counts[]  = {0, 1, 255, 256, 257, 513};
    static const unsigned budgets[] = {1, 2, 3, 17, 255, 256, 257, 4096};
    unsigned copy, width, flags, n, b;
    for (copy = 0; copy < 2; ++copy)
        for (width = 1; width <= 4; width *= 2)
            for (flags = 0; flags < 8; ++flags)
                for (n = 0; n < 6; ++n)
                    for (b = 0; b < 8; ++b)
                        test(copy, width, flags, counts[n], budgets[b], (n + b) % 8);
#ifdef X86EMU_BULK_STRINGS
    if (!bulk_calls || bulk_largest != X86_BULK_MAX || span_largest <= X86_BULK_MAX || span_calls > bulk_calls)
        ++failures;
#else
    if (bulk_calls || span_calls)
        ++failures;
#endif
    printf("BULK RESULT cases=%u failures=%u callbacks=%u span_callbacks=%u span_largest=%u\n", cases, failures,
           bulk_calls, span_calls, span_largest);
    return failures ? 20 : 0;
}
