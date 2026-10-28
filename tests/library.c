#include <proto/exec.h>
#include <proto/x86emu.h>
#include <stdio.h>
#include <string.h>
struct Library *X86EmuBase;
static unsigned checks, failures, bulk_calls, scan_calls, bulk_largest, data_writes;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
static unsigned char ram[65536];
static uint32_t X86_CALLBACK access(void *user, uint32_t address, uint32_t width, uint32_t op, uint32_t *value)
{
    unsigned char *mem = user;
    uint32_t i;
    if (op > X86_FETCH || address > sizeof(ram) || width > sizeof(ram) - address)
        return 1;
    if (op == X86_WRITE)
        ++data_writes;
    if (op == X86_WRITE)
        for (i = 0; i < width; ++i)
            mem[address + i] = *value >> (8 * i);
    else {
        *value = 0;
        for (i = 0; i < width; ++i)
            *value |= (uint32_t)mem[address + i] << (8 * i);
    }
    return 0;
}
static uint32_t X86_CALLBACK bulk(void *user, const struct X86BulkRequest *b)
{
    uint32_t i, value = b->value;
    if (b->operation != X86_BULK_FILL)
        return 0;
    ++bulk_calls;
    if (b->count > bulk_largest)
        bulk_largest = b->count;
    for (i = 0; i < b->count; ++i)
        if (access(user, b->destination + i * b->stride, b->width, X86_WRITE, &value))
            break;
    return i;
}
static uint32_t X86_CALLBACK scan(void *user, const struct X86ScanRequest *s, uint32_t *last)
{
    uint32_t i;
    ++scan_calls;
    for (i = 0; i < s->count;) {
        if (access(user, s->address + i * s->stride, s->width, X86_READ, last))
            break;
        ++i;
        if ((*last == s->expected) != s->repeat_equal)
            break;
    }
    return i;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct Library *again;
    X86Context *ctx;
    struct X86Config config    = {.size = sizeof(config), .version = X86EMU_ABI_VERSION, .access = access, .user = ram};
    struct X86Registers regs   = {.size = sizeof(regs), .esp = 0xff00, .eip = 0x1000};
    struct X86Limits limits    = {sizeof(limits), 100, 1000, 1000, NULL};
    struct X86Result result    = {.size = sizeof(result)};
    const unsigned char code[] = {0x66, 0xb8, 0x78, 0x56, 0x34, 0x12, 0x66, 0xa3, 0x01, 0x20, 0xf4};
    X86EmuBase                 = OpenLibrary((STRPTR) "x86emu.library", X86EMU_VERSION);
    CHECK(X86EmuBase != NULL);
    if (!X86EmuBase)
        return 20;
    printf("LIBRARY %s\n", (char *)X86EmuBase->lib_IdString);
    CHECK(X86EmuBase->lib_NegSize == 60);
    again = OpenLibrary((STRPTR) "x86emu.library", X86EMU_VERSION);
    CHECK(again == X86EmuBase);
    CHECK(X86EmuBase->lib_OpenCnt == 2);
    if (again)
        CloseLibrary(again);
    CHECK(X86EmuBase->lib_OpenCnt == 1);
    config.version = 99;
    CHECK(X86Create(&config) == NULL);
    config.version = X86EMU_ABI_VERSION;
    --config.size;
    CHECK(X86Create(&config) == NULL);
    ++config.size;
    ctx = X86Create(&config);
    CHECK(ctx != NULL);
    if (ctx) {
        memcpy(ram + 0x1000, code, sizeof(code));
        CHECK(X86SetRegisters(ctx, &regs) == 0);
        CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
        CHECK(X86GetRegisters(ctx, &regs) == 0);
        CHECK(regs.eax == 0x12345678);
        CHECK(ram[0x2001] == 0x78 && ram[0x2004] == 0x12);
        CHECK(X86Delete(ctx) == 0);
    }
    for (unsigned batching = 0; batching < 2; ++batching) {
        struct X86Config extended = config;
        extended.bulk             = batching ? bulk : NULL;
        ctx                       = X86Create(&extended);
        CHECK(ctx != NULL);
        if (ctx) {
            ram[0x1000] = 0xf3;
            ram[0x1001] = 0xaa;
            ram[0x1002] = 0xf4;
            memset(ram + 0x3000, 0, 33);
            regs.eip = 0x1000;
            regs.edi = 0x3000;
            regs.eax = 0x5a;
            regs.ecx = 33;
            CHECK(X86SetRegisters(ctx, &regs) == 0);
            CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
            CHECK(ram[0x3000] == 0x5a && ram[0x3020] == 0x5a);
#ifdef X86EMU_BULK_STRINGS
            CHECK(batching ? bulk_calls > 0 : bulk_calls == 0);
#else
            CHECK(bulk_calls == 0);
#endif
            CHECK(X86Delete(ctx) == 0);
        }
    }
    for (unsigned scanning = 0; scanning < 2; ++scanning) {
        struct X86Config extended = config;
        extended.scan             = scanning ? scan : NULL;
        ctx                       = X86Create(&extended);
        CHECK(ctx != NULL);
        if (ctx) {
            ram[0x1000] = 0xf3;
            ram[0x1001] = 0xae;
            ram[0x1002] = 0xf4;
            memset(ram + 0x3000, 0x5a, 33);
            ram[0x3010] = 0;
            regs.eip    = 0x1000;
            regs.edi    = 0x3000;
            regs.eax    = 0x5a;
            regs.ecx    = 33;
            regs.eflags = 2;
            CHECK(X86SetRegisters(ctx, &regs) == 0);
            CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
            CHECK(X86GetRegisters(ctx, &regs) == 0);
            CHECK(regs.ecx == 16 && regs.edi == 0x3011 && !(regs.eflags & 0x40));
#ifdef X86EMU_BULK_STRINGS
            CHECK(scanning ? scan_calls > 0 : scan_calls == 0);
#else
            CHECK(scan_calls == 0);
#endif
            CHECK(X86Delete(ctx) == 0);
        }
    }
    {
        struct X86Config extended = config;
        extended.bulk             = bulk;
        extended.bulk_elements    = X86_SPAN_MAX;
        ctx                       = X86Create(&extended);
        CHECK(ctx != NULL);
        if (ctx) {
            ram[0x1000] = 0xf3;
            ram[0x1001] = 0xaa;
            ram[0x1002] = 0xf4;
            memset(ram + 0x3000, 0, 701);
            bulk_largest = 0;
            regs.eip     = 0x1000;
            regs.edi     = 0x3000;
            regs.eax     = 0x5a;
            regs.ecx     = 700;
            regs.eflags  = 2;
            CHECK(X86SetRegisters(ctx, &regs) == 0);
            CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
            CHECK(ram[0x3000] == 0x5a && ram[0x3000 + 699] == 0x5a && ram[0x3000 + 700] == 0);
            CHECK(result.accesses > 700);
#ifdef X86EMU_BULK_STRINGS
            CHECK(bulk_largest == 700);
#else
            CHECK(bulk_largest == 0);
#endif
            CHECK(X86Delete(ctx) == 0);
        }
        extended.bulk_elements = X86_SPAN_MAX + 1;
        CHECK(X86Create(&extended) == NULL);
    }
    {
        /* Direct data regions: the callback no longer sees mapped RAM writes. */
        struct X86CodeRegion regions[] = {{0, 0x4000, ram}, {0x4000, 0x4000, ram + 0x4000}};
        struct X86Config extended      = config;
        extended.region_count          = 2;
        extended.regions               = regions;
        extended.data_regions          = 4;
        CHECK(X86Create(&extended) == NULL);
        extended.data_regions = 1;
        ctx                   = X86Create(&extended);
        CHECK(ctx != NULL);
        if (ctx) {
            memcpy(ram + 0x1000, code, sizeof(code));
            memset(ram + 0x2000, 0, 8);
            data_writes = 0;
            regs.eip    = 0x1000;
            regs.eflags = 2;
            CHECK(X86SetRegisters(ctx, &regs) == 0);
            CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
            CHECK(ram[0x2001] == 0x78 && ram[0x2004] == 0x12 && data_writes == 0);
            /* The same store into the fetch-only region uses the callback. */
            ram[0x1008] = 0x01;
            ram[0x1009] = 0x40;
            data_writes = 0;
            CHECK(X86SetRegisters(ctx, &regs) == 0);
            CHECK(X86Run(ctx, &limits, &result) == X86_STOP_HALT);
            CHECK(ram[0x4001] == 0x78 && ram[0x4004] == 0x12 && data_writes == 1);
            CHECK(X86Delete(ctx) == 0);
        }
    }
    printf("CONTEXT bytes=%lu\n", (unsigned long)X86ContextBytes());
    CloseLibrary(X86EmuBase);
    X86EmuBase = OpenLibrary((STRPTR) "x86emu.library", X86EMU_VERSION);
    CHECK(X86EmuBase != NULL);
    if (X86EmuBase)
        CloseLibrary(X86EmuBase);
    printf("LIBRARY RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
