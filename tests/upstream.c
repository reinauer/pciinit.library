/* Portable runner for libx86emu's generated fixtures. Private CPU access is
 * intentional: upstream tests include hidden descriptor and control state
 * outside the real-mode public API. Both backends share this state layout. */
#include "private/libx86emu_backend.h"
#include "private/m68kjit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Field
{
    const char *name;
    unsigned offset, bytes;
};
#define FIELD(name, member) {name, offsetof(x86emu_t, x86.member), sizeof(((x86emu_t *)0)->x86.member)}
#define SEG(s, S) \
    FIELD(#s, R_##S), FIELD(#s ".base", R_##S##_BASE), FIELD(#s ".limit", R_##S##_LIMIT), FIELD(#s ".acc", R_##S##_ACC)
static const struct Field fields[] = {FIELD("eax", R_EAX),
                                      FIELD("ebx", R_EBX),
                                      FIELD("ecx", R_ECX),
                                      FIELD("edx", R_EDX),
                                      FIELD("esi", R_ESI),
                                      FIELD("edi", R_EDI),
                                      FIELD("ebp", R_EBP),
                                      FIELD("esp", R_ESP),
                                      FIELD("eip", R_EIP),
                                      FIELD("eflags", R_EFLG),
                                      SEG(cs, CS),
                                      SEG(ss, SS),
                                      SEG(ds, DS),
                                      SEG(es, ES),
                                      SEG(fs, FS),
                                      SEG(gs, GS),
                                      SEG(tr, TR),
                                      SEG(ldt, LDT),
                                      FIELD("gdt.base", R_GDT_BASE),
                                      FIELD("gdt.limit", R_GDT_LIMIT),
                                      FIELD("idt.base", R_IDT_BASE),
                                      FIELD("idt.limit", R_IDT_LIMIT),
                                      FIELD("cr0", R_CR0),
                                      FIELD("cr1", R_CR1),
                                      FIELD("cr2", R_CR2),
                                      FIELD("cr3", R_CR3),
                                      FIELD("cr4", R_CR4),
                                      FIELD("dr0", R_DR0),
                                      FIELD("dr1", R_DR1),
                                      FIELD("dr2", R_DR2),
                                      FIELD("dr3", R_DR3),
                                      FIELD("dr6", R_DR6),
                                      FIELD("dr7", R_DR7)};
#define RAM_BYTES 0x100000
static uint8_t ram[RAM_BYTES];
struct Page
{
    uint32_t base;
    uint8_t data[4096];
};
static struct Page *pages[256];
static unsigned page_count;
static uint8_t *byte(uint32_t address)
{
    unsigned i;
    if (address < RAM_BYTES)
        return ram + address;
    for (i = 0; i < page_count; ++i)
        if (pages[i]->base == (address & ~4095U))
            return pages[i]->data + (address & 4095);
    if (page_count == 256)
        exit(25);
    pages[page_count] = calloc(1, sizeof(struct Page));
    if (!pages[page_count])
        exit(25);
    pages[page_count]->base = address & ~4095U;
    return pages[page_count++]->data + (address & 4095);
}
static uint32_t X86_CALLBACK access_memory(void *user, uint32_t address, uint32_t width, uint32_t op, uint32_t *value)
{
    unsigned i;
    (void)user;
    if (op == X86_IN) {
        *value = 0;
        return 0;
    }
    if (op == X86_OUT)
        return 0;
    if (op == X86_WRITE)
        for (i = 0; i < width; ++i)
            *byte(address + i) = *value >> (i * 8);
    else {
        *value = 0;
        for (i = 0; i < width; ++i)
            *value |= (uint32_t)*byte(address + i) << (i * 8);
    }
    return 0;
}
static uint32_t get(FILE *f)
{
    uint32_t v = 0;
    unsigned i;
    int b;
    for (i = 0; i < 4; ++i) {
        b = fgetc(f);
        if (b < 0)
            exit(21);
        v = (v << 8) | b;
    }
    return v;
}
static void put(FILE *f, uint32_t v)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        if (fputc((v >> (24 - 8 * i)) & 255, f) == EOF)
            exit(22);
}
static uint32_t read_field(x86emu_t *cpu, unsigned i)
{
    void *p = (uint8_t *)cpu + fields[i].offset;
    return fields[i].bytes == 2 ? *(uint16_t *)p : *(uint32_t *)p;
}
static unsigned fixture(x86emu_t *cpu, const char *path, int verify)
{
    FILE *f = fopen(path, "rb");
    unsigned n, i, j, errors = 0;
    if (!f)
        exit(21);
    n = get(f);
    for (i = 0; i < n; ++i) {
        char name[32];
        uint32_t v;
        unsigned len = get(f);
        if (len >= sizeof(name) || fread(name, 1, len, f) != len)
            exit(21);
        name[len] = 0;
        v         = get(f);
        for (j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j)
            if (!strcmp(name, fields[j].name))
                break;
        if (j == sizeof(fields) / sizeof(fields[0]))
            exit(21);
        if (verify) {
            if (read_field(cpu, j) != v)
                ++errors;
        } else {
            void *p = (uint8_t *)cpu + fields[j].offset;
            if (fields[j].bytes == 2)
                *(uint16_t *)p = v;
            else
                *(uint32_t *)p = v;
        }
    }
    n = get(f);
    for (i = 0; i < n; ++i) {
        uint32_t a = get(f), v = get(f);
        if (verify) {
            if (*byte(a) != v)
                ++errors;
        } else
            *byte(a) = v;
    }
    fclose(f);
    return errors;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    char name[256], path[512];
    FILE *manifest;
    unsigned tests = 0, total = 0;
    if (argc != 3 || !(manifest = fopen(argv[1], "r")))
        return 20;
    while (fgets(name, sizeof(name), manifest)) {
        struct X86CodeRegion region = {0, sizeof(ram), ram};
        struct X86Config config     = {.size         = sizeof(config),
                                       .version      = X86EMU_ABI_VERSION,
                                       .access       = access_memory,
                                       .region_count = 1,
                                       .regions      = &region};
        struct X86Limits limits     = {sizeof(limits), 2000000, 20000000, 0, NULL};
        struct X86Result result     = {.size = sizeof(result)};
        struct X86JitStats stats    = {0};
        X86Context *ctx;
        x86emu_t *cpu;
        FILE *out;
        unsigned i, errors;
        name[strcspn(name, "\r\n")] = 0;
        memset(ram, 0, sizeof(ram));
        while (page_count)
            free(pages[--page_count]);
        ctx = xe_create(&config);
        if (!ctx)
            return 20;
        cpu = &((struct LibX86State *)xe_backend_data(ctx))->cpu;
        snprintf(path, sizeof(path), "%s%s.input", argv[2], name);
        fixture(cpu, path, 0);
        xe_run(ctx, &limits, &result);
        snprintf(path, sizeof(path), "%s%s.golden", argv[2], name);
        errors = fixture(cpu, path, 1);
        snprintf(path, sizeof(path), "%s%s.output", argv[2], name);
        out = fopen(path, "wb");
        if (!out)
            return 22;
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
            put(out, read_field(cpu, i));
        put(out, result.reason);
        put(out, result.instructions);
        put(out, result.accesses);
        put(out, result.address);
        put(out, result.operation);
        put(out, result.interrupt);
        put(out, cpu->x86.R_TSC >> 32);
        put(out, cpu->x86.R_TSC);
        if (fwrite(ram, 1, sizeof(ram), out) != sizeof(ram))
            return 22;
        /* Page order is deterministic for identical guest memory accesses. */
        for (i = 0; i < page_count; ++i) {
            put(out, pages[i]->base);
            if (fwrite(pages[i]->data, 1, 4096, out) != 4096)
                return 22;
        }
        if (fclose(out))
            return 22;
#ifdef TEST_JIT
        xe_jit_stats(ctx, &stats);
#endif
        printf("UPSTREAM %s stop=%lu instructions=%lu golden_errors=%u native=%lu blocks=%lu fallback=%lu\n", name,
               (unsigned long)result.reason, (unsigned long)result.instructions, errors,
               (unsigned long)stats.instructions, (unsigned long)stats.blocks, (unsigned long)stats.fallback);
        total += stats.instructions;
        ++tests;
        xe_delete(ctx);
    }
    fclose(manifest);
    while (page_count)
        free(pages[--page_count]);
    printf("UPSTREAM RESULT tests=%u native=%u\n", tests, total);
    return 0;
}
