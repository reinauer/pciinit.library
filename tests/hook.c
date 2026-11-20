/* Access hook and bus-error policy against RAM stand-ins for the card:
 * I/O aperture, legacy window and one memory BAR are buffers, and the
 * configuration space comes from a fake openpci.library jump table. */
#include "private/pciinit.h"
#include <proto/exec.h>
#include <stdio.h>
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

static uint8_t io[0x10000], legacy[PI_LEGACY_BYTES], bar[4096], config_space[256];
static PCIInitContext ctx;

/* openpci reads and writes configuration space little-endian. */
static ULONG cfg_get(ULONG reg, unsigned width)
{
    ULONG v = 0;
    unsigned i;
    for (i = 0; i < width; ++i)
        v |= (ULONG)config_space[(reg + i) & 255] << (8 * i);
    return v;
}
static void cfg_put(ULONG reg, ULONG value, unsigned width)
{
    unsigned i;
    for (i = 0; i < width; ++i)
        config_space[(reg + i) & 255] = value >> (8 * i);
}
#define PCI_ARGS  REG(ULONG, reg, d0), REG(struct pci_dev *, p, a0), REG(struct Library *, b, a6)
#define PCI_WARGS REG(ULONG, reg, d0), REG(ULONG, value, d1), REG(struct pci_dev *, p, a0), REG(struct Library *, b, a6)
static ULONG read8(PCI_ARGS)
{
    (void)p, (void)b;
    return cfg_get(reg, 1);
}
static ULONG read16(PCI_ARGS)
{
    (void)p, (void)b;
    return cfg_get(reg, 2);
}
static ULONG read32(PCI_ARGS)
{
    (void)p, (void)b;
    return cfg_get(reg, 4);
}
static void write8(PCI_WARGS)
{
    (void)p, (void)b;
    cfg_put(reg, value, 1);
}
static void write16(PCI_WARGS)
{
    (void)p, (void)b;
    cfg_put(reg, value, 2);
}
static void write32(PCI_WARGS)
{
    (void)p, (void)b;
    cfg_put(reg, value, 4);
}
/* Vectors at -108..-138: JMP absolute to the functions above. */
static uint16_t openpci[(144 + sizeof(struct Library)) / 2];
static struct Library *fake_openpci(void)
{
    static const struct
    {
        unsigned offset;
        APTR entry;
    } vectors[]   = {{108, (APTR)read8},  {114, (APTR)read16},  {120, (APTR)read32},
                     {126, (APTR)write8}, {132, (APTR)write16}, {138, (APTR)write32}};
    uint8_t *base = (uint8_t *)openpci + 144;
    unsigned i;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        uint8_t *v = base - vectors[i].offset;
        ULONG a    = (ULONG)vectors[i].entry;
        v[0]       = 0x4e;
        v[1]       = 0xf9;
        v[2]       = a >> 24;
        v[3]       = a >> 16;
        v[4]       = a >> 8;
        v[5]       = a;
    }
    CacheClearU();
    return (struct Library *)base;
}

/* The hook records every call and answers as the case asks. */
static struct PCIInitAccessMsg seen[8];
static unsigned calls;
static uint32_t before_verdict, before_value, after_verdict, after_value;
static int replace_before, replace_after;
static ULONG hook_entry(REG(struct Hook *, h, a0), REG(APTR, object, a2), REG(struct PCIInitAccessMsg *, m, a1))
{
    (void)h;
    if (object != &ctx || m->size != sizeof(*m))
        return PI_HOOK_FAULT;
    if (calls < 8)
        seen[calls] = *m;
    ++calls;
    if (m->phase == PI_PHASE_BEFORE) {
        if (replace_before)
            m->value = before_value;
        return before_verdict;
    }
    if (replace_after)
        m->value = after_value;
    return after_verdict;
}
static struct Hook hook = {{NULL, NULL}, (ULONG (*)())(APTR)hook_entry, NULL, NULL};
static struct PCIInitTraceEntry trace[4];
static struct PCIInitDiagnostics diag = {.size = sizeof(diag), .capacity = 4, .entries = trace};

static void reset(int hooked)
{
    calls          = 0;
    before_verdict = after_verdict = PI_HOOK_PASS;
    replace_before = replace_after = 0;
    ctx.hook                       = hooked ? &hook : NULL;
    ctx.decode                     = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    ctx.range_count                = 0;
    ctx.stop_bus_error             = 0;
    pi_hardware_flags(&ctx);
    ctx.trap.faults = ctx.faults_seen = 0;
    diag.count                        = 0;
    memset(seen, 0, sizeof(seen));
}
static uint32_t access(uint32_t address, uint32_t width, uint32_t op, uint32_t value, uint32_t *result)
{
    uint32_t status = pi_hardware_access(&ctx, address, width, op, &value);
    if (result)
        *result = value;
    return status;
}

static void plain_case(void)
{
    uint32_t v;
    reset(0);
    CHECK(access(0x8180, 4, X86_OUT, 0x11223344, NULL) == 0);
    CHECK(io[0x8180] == 0x44 && io[0x8181] == 0x33 && io[0x8182] == 0x22 && io[0x8183] == 0x11);
    CHECK(access(0x8180, 4, X86_IN, 0, &v) == 0 && v == 0x11223344);
    /* VGA ports take adjacent byte cycles. */
    CHECK(access(0x3c4, 2, X86_OUT, 0x0102, NULL) == 0 && io[0x3c4] == 0x02 && io[0x3c5] == 0x01);
    legacy[0x10] = 0x34;
    legacy[0x11] = 0x12;
    CHECK(access(0xa0010, 2, X86_READ, 0, &v) == 0 && v == 0x1234);
    CHECK(access(0xe0000100, 4, X86_WRITE, 0xa1b2c3d4, NULL) == 0 && bar[0x100] == 0xd4 && bar[0x103] == 0xa1);
    CHECK(diag.count == 5 && !(trace[(diag.count - 1) % 4].operation & PI_TRACE_HOOKED));
}

static void message_case(void)
{
    uint32_t v;
    reset(1);
    CHECK(access(0xe0000204, 2, X86_WRITE, 0xbeef, NULL) == 0 && calls == 1);
    CHECK(seen[0].space == PI_SPACE_BAR && seen[0].bar == 2 && seen[0].offset == 0x204 && seen[0].width == 2 &&
          seen[0].write == 1 && seen[0].value == 0xbeef && seen[0].phase == PI_PHASE_BEFORE);
    CHECK(bar[0x204] == 0xef && bar[0x205] == 0xbe);
    reset(1);
    CHECK(access(0xa0010, 2, X86_READ, 0, &v) == 0 && calls == 2 && v == 0x1234);
    CHECK(seen[0].space == PI_SPACE_LEGACY && seen[0].offset == 0x10 && !seen[0].write);
    CHECK(seen[1].phase == PI_PHASE_AFTER && seen[1].value == 0x1234);
    reset(1);
    CHECK(access(0x3d4, 1, X86_IN, 0, &v) == 0 && seen[0].space == PI_SPACE_IO && seen[0].offset == 0x3d4);
    /* Platform ports are not the card's and never reach the hook. */
    reset(1);
    CHECK(access(0x61, 1, X86_IN, 0, &v) == 0 && calls == 0);
    CHECK(access(0x80, 1, X86_OUT, 0x55, NULL) == 0 && calls == 0);
}

static void verdict_case(void)
{
    uint32_t v;
    /* A write's value is replaced on its way out. */
    reset(1);
    replace_before = 1;
    before_value   = 0x55667788;
    CHECK(access(0x8180, 4, X86_OUT, 0x11223344, NULL) == 0);
    CHECK(io[0x8180] == 0x88 && io[0x8183] == 0x55);
    CHECK(trace[(diag.count - 1) % 4].operation == (X86_OUT | PI_TRACE_HOOKED));
    CHECK(trace[(diag.count - 1) % 4].value == 0x55667788);
    /* A read is adjusted after the card answered. */
    reset(1);
    replace_after = 1;
    after_value   = 0x34;
    CHECK(access(0xa0010, 2, X86_READ, 0, &v) == 0 && v == 0x34);
    CHECK(trace[(diag.count - 1) % 4].operation == (X86_READ | PI_TRACE_HOOKED));
    /* DONE completes a read without the card and without AFTER. */
    reset(1);
    before_verdict = PI_HOOK_DONE;
    replace_before = 1;
    before_value   = 0xdeadbeef;
    bar[0x300]     = 0x11;
    CHECK(access(0xe0000300, 4, X86_READ, 0, &v) == 0 && v == 0xdeadbeef && calls == 1);
    /* DONE drops a write. */
    reset(1);
    before_verdict = PI_HOOK_DONE;
    CHECK(access(0xe0000300, 1, X86_WRITE, 0x99, NULL) == 0 && bar[0x300] == 0x11);
    CHECK(trace[(diag.count - 1) % 4].operation & PI_TRACE_HOOKED);
    /* FAULT stops the run in either phase. */
    reset(1);
    before_verdict = PI_HOOK_FAULT;
    CHECK(access(0x8180, 1, X86_OUT, 1, NULL) == 1);
    reset(1);
    after_verdict = PI_HOOK_FAULT;
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 1);
    /* An unknown verdict counts as FAULT. */
    reset(1);
    before_verdict = 7;
    CHECK(access(0x8180, 1, X86_OUT, 1, NULL) == 1);
    /* The hook may complete accesses to a BAR the bridge hides. */
    reset(1);
    ctx.bars_visible = 0;
    CHECK(access(0xe0000300, 1, X86_READ, 0, &v) == 1);
    before_verdict = PI_HOOK_DONE;
    replace_before = 1;
    before_value   = 0x42;
    CHECK(access(0xe0000300, 1, X86_READ, 0, &v) == 0 && v == 0x42);
    ctx.bars_visible = 1;
}

static void range_case(void)
{
    uint32_t v;
    reset(1);
    ctx.ranges[0]   = (struct PCIInitAccessRange){PI_SPACE_IO, 0, 0x3c0, 0x3cf};
    ctx.ranges[1]   = (struct PCIInitAccessRange){PI_SPACE_BAR, 2, 0xa0, 0xa3};
    ctx.range_count = 2;
    CHECK(access(0x3d4, 1, X86_IN, 0, &v) == 0 && calls == 0);
    CHECK(access(0x3cf, 2, X86_OUT, 0, NULL) == 0 && calls == 1);
    CHECK(access(0xe00000a4, 4, X86_READ, 0, &v) == 0 && calls == 1);
    CHECK(access(0xe00000a0, 4, X86_READ, 0, &v) == 0 && calls == 3);
    CHECK(access(0xa00a0, 4, X86_READ, 0, &v) == 0 && calls == 3);
    ctx.ranges[1].bar = 1;
    CHECK(access(0xe00000a0, 4, X86_READ, 0, &v) == 0 && calls == 3);
}

static void fast_path_case(void)
{
    const uint8_t source[4]       = {1, 2, 3, 4};
    struct X86BulkRequest fill    = {sizeof(fill), X86_BULK_FILL, 0, 0xa0100, 2, 4, 0x1234, 2};
    struct X86BulkRequest copy    = {sizeof(copy), X86_BULK_COPY, 0, 0xa0200, 1, 4, 0, 1};
    struct X86ScanRequest scan    = {sizeof(scan), 0xa0100, 2, 4, 0x1234, 1, 2};
    uint32_t last                 = 0;
    struct PCIInitDiagnostics *d0 = ctx.diagnostics;
    ctx.diagnostics               = NULL; /* scans are scalar while tracing */
    reset(0);
    CHECK(pi_hardware_bulk(&ctx, &fill, NULL) == 4);
    CHECK(legacy[0x100] == 0x34 && legacy[0x101] == 0x12 && legacy[0x106] == 0x34 && legacy[0x107] == 0x12);
    CHECK(pi_hardware_bulk(&ctx, &copy, source) == 4 && !memcmp(legacy + 0x200, source, 4));
    CHECK(pi_hardware_scan(&ctx, &scan, &last) == 4 && last == 0x1234);
    /* A hooked range sends overlapping requests to the scalar path. */
    reset(1);
    ctx.ranges[0]   = (struct PCIInitAccessRange){PI_SPACE_LEGACY, 0, 0x106, 0x106};
    ctx.range_count = 1;
    CHECK(pi_hardware_bulk(&ctx, &fill, NULL) == 0);
    CHECK(pi_hardware_scan(&ctx, &scan, &last) == 0);
    CHECK(pi_hardware_bulk(&ctx, &copy, source) == 4 && calls == 0);
    /* So does strict bus-error mode. */
    reset(0);
    ctx.stop_bus_error = 1;
    pi_hardware_flags(&ctx);
    CHECK(pi_hardware_bulk(&ctx, &copy, source) == 0);
    CHECK(pi_hardware_scan(&ctx, &scan, &last) == 0);
    /* Without the window every element reads zero. */
    reset(0);
    {
        struct X86ScanRequest zeros = {sizeof(zeros), 0xa0000, 4, 8, 0, 1, 4};
        APTR window                 = ctx.legacy;
        ctx.legacy                  = NULL;
        ctx.legacy_missed           = 0;
        last                        = 1;
        CHECK(pi_hardware_scan(&ctx, &zeros, &last) == 8 && last == 0 && ctx.legacy_missed);
        ctx.legacy = window;
    }
    ctx.diagnostics = d0;
}

static void config_case(void)
{
    uint32_t v, cf8 = 0x80000000U | (ctx.bdf << 8);
    reset(1);
    config_space[0] = 0x02;
    config_space[1] = 0x10;
    CHECK(access(0xcf8, 4, X86_OUT, cf8, NULL) == 0 && calls == 0);
    CHECK(access(0xcfc, 2, X86_IN, 0, &v) == 0 && v == 0x1002 && calls == 2);
    CHECK(seen[0].space == PI_SPACE_CONFIG && seen[0].offset == 0 && seen[0].width == 2);
    /* The refusal filter still applies to passed writes... */
    reset(1);
    CHECK(access(0xcf8, 4, X86_OUT, cf8 | 0x0c, NULL) == 0);
    CHECK(access(0xcfd, 1, X86_OUT, 0x40, NULL) == 1 && config_space[0x0d] == 0 && calls == 1);
    CHECK(seen[0].offset == 0x0d);
    /* ...but a hook may complete one itself. */
    reset(1);
    before_verdict = PI_HOOK_DONE;
    CHECK(access(0xcfd, 1, X86_OUT, 0x40, NULL) == 0 && config_space[0x0d] == 0);
    /* Bus mastering stays off: only I/O and memory enables pass. */
    reset(1);
    CHECK(access(0xcf8, 4, X86_OUT, cf8 | 4, NULL) == 0);
    CHECK(access(0xcfc, 2, X86_OUT, 7, NULL) == 1 && config_space[4] == 0);
    CHECK(access(0xcfc, 2, X86_OUT, 3, NULL) == 0 && config_space[4] == 3);
}

/* With a decode turned off by the ROM, the access is a master abort the
 * library completes: reads return all-ones, writes are dropped. */
static void decode_case(void)
{
    const uint8_t source[4]    = {1, 2, 3, 4};
    struct X86BulkRequest copy = {sizeof(copy), X86_BULK_COPY, 0, 0xa0300, 1, 4, 0, 1};
    struct X86ScanRequest scan = {sizeof(scan), 0xa0100, 2, 4, 0x1234, 1, 2};
    uint32_t v, last, cf8 = 0x80000000U | (ctx.bdf << 8) | PCI_COMMAND;
    struct PCIInitDiagnostics *d0 = ctx.diagnostics;
    reset(0);
    io[0x8180] = 0x5a;
    CHECK(access(0xcf8, 4, X86_OUT, cf8, NULL) == 0);
    CHECK(access(0xcfc, 2, X86_OUT, PCI_COMMAND_IO, NULL) == 0 && config_space[4] == 1 && ctx.detour);
    CHECK(access(0xa0010, 2, X86_READ, 0, &v) == 0 && v == 0xffff);
    CHECK(access(0xa0020, 1, X86_WRITE, 0x77, NULL) == 0 && legacy[0x20] == 0);
    CHECK(access(0xe0000100, 4, X86_READ, 0, &v) == 0 && v == 0xffffffffU);
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 0 && v == 0x5a);
    ctx.diagnostics = NULL;
    CHECK(pi_hardware_bulk(&ctx, &copy, source) == 0 && legacy[0x300] == 0);
    CHECK(pi_hardware_scan(&ctx, &scan, &last) == 0);
    ctx.diagnostics = d0;
    CHECK(access(0xcfc, 1, X86_OUT, PCI_COMMAND_MEMORY, NULL) == 0 && config_space[4] == 2);
    CHECK(access(0x8180, 4, X86_IN, 0, &v) == 0 && v == 0xffffffffU);
    CHECK(access(0x3c4, 2, X86_IN, 0, &v) == 0 && v == 0xffff);
    CHECK(access(0x8181, 1, X86_OUT, 0x66, NULL) == 0 && io[0x8181] != 0x66);
    CHECK(access(0xa0010, 2, X86_READ, 0, &v) == 0 && v == 0x1234);
    /* The hook still sees the access first and may complete it. */
    reset(1);
    ctx.decode = PCI_COMMAND_MEMORY;
    pi_hardware_flags(&ctx);
    before_verdict = PI_HOOK_DONE;
    replace_before = 1;
    before_value   = 0x42;
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 0 && v == 0x42 && calls == 1);
    reset(0);
    ctx.decode = 0;
    pi_hardware_flags(&ctx);
    CHECK(access(0xcfc, 2, X86_OUT, PCI_COMMAND_IO | PCI_COMMAND_MEMORY, NULL) == 0 && !ctx.detour);
}

static void bus_error_case(void)
{
    uint32_t v;
    reset(0);
    /* A fault the trap handler caught counts once against the access. */
    ctx.trap.faults = 1;
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 0);
    ctx.stop_bus_error = 1;
    pi_hardware_flags(&ctx);
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 1 && ctx.faults_seen == 1);
    CHECK(access(0x8180, 1, X86_IN, 0, &v) == 0);
}

/* Bulk routines past the 16-bit DBF count, and their ends. */
static void bulk_routine_case(void)
{
    static uint16_t big[65540];
    static uint8_t source[65540 * 2];
    unsigned i, ok = 1;
    for (i = 0; i < sizeof(source); ++i)
        source[i] = i * 7;
    big[65537] = 0x1111;
    big[65538] = 0x2222;
    pi_bus_fill16(big, 0xa55a, 65537);
    CHECK(big[0] == 0xa55a && big[65535] == 0xa55a && big[65536] == 0xa55a && big[65537] == 0x1111);
    pi_bus_fill8(big, 0x3c, 1);
    CHECK(big[0] == 0x3c5a);
    pi_bus_copy8(big, source, 65538);
    for (i = 0; i < 65538; ++i)
        ok &= ((uint8_t *)big)[i] == source[i];
    CHECK(ok && ((uint8_t *)big)[65538] == 0xa5);
    pi_bus_fill32(big, 0, 32769);
    CHECK(big[65537] == 0 && big[65538] == 0x2222);
    pi_bus_copy16(big, source, 65538);
    CHECK(!memcmp(big, source, 65538 * 2) && big[65538] == 0x2222);
    pi_bus_copy32(big, source + 4, 2);
    CHECK(!memcmp(big, source + 4, 8) && big[4] == ((source[8] << 8) | source[9]));
}

int main(void)
{
    ctx.hardware     = 1;
    ctx.board        = (struct pci_dev *)1;
    ctx.pci          = fake_openpci();
    ctx.bdf          = 0x0108;
    ctx.io           = (uint32_t)io;
    ctx.legacy       = legacy;
    ctx.bars[0]      = (struct PIBar){0xe0000000U, (uint32_t)bar, sizeof(bar), 2};
    ctx.bar_count    = 1;
    ctx.bars_visible = 1;
    ctx.diagnostics  = &diag;
    plain_case();
    /* The 68040/060 path through the bus routines. */
    ctx.bus_routines = 1;
    plain_case();
    ctx.bus_routines = 0;
    bulk_routine_case();
    message_case();
    verdict_case();
    range_case();
    fast_path_case();
    config_case();
    decode_case();
    bus_error_case();
    printf("HOOK RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
