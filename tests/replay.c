/* Deterministic CPU replay of a private PCI recording, without hardware.
 * Validate every device operation, interrupt, final register and RAM byte.
 * This measures engine work, not PCI latency or firmware delay accuracy. */
#include "private/libx86emu_backend.h"
#include "private/record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RAM_SIZE 0x100000U
struct Event
{
    uint32_t kind, bytes;
    const uint8_t *data, *next;
};
static uint8_t ram[RAM_SIZE];
static const uint8_t *cursor, *end;
static uint32_t bulk_index, events, failed, profile_pc[65536], profile_other;
static uint32_t accesses[5], widths[3], bulk_calls, bulk_elements;
static int profiling;
static int (*original_check)(x86emu_t *);
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void error(const char *s)
{
    if (!failed)
        fprintf(stderr, "REPLAY mismatch event %lu: %s\n", (unsigned long)events, s);
    failed = 1;
}
static struct Event peek(void)
{
    struct Event e = {0};
    if (!cursor || cursor > end || end - cursor < 8) {
        error("missing event");
        return e;
    }
    e.kind  = be32(cursor);
    e.bytes = be32(cursor + 4);
    e.data  = cursor + 8;
    if (e.bytes > (uint32_t)(end - e.data)) {
        error("truncated event");
        e.kind = 0;
        return e;
    }
    e.next = e.data + e.bytes;
    return e;
}
static void pop(struct Event e)
{
    cursor = e.next;
    ++events;
    bulk_index = 0;
}
static uint32_t mask(unsigned width)
{
    return width == 4 ? UINT32_MAX : (1U << (8 * width)) - 1;
}
static uint32_t little(const uint8_t *p, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static int ordinary(uint32_t a, uint32_t bytes, int write)
{
    return a < RAM_SIZE && bytes <= RAM_SIZE - a && !(a < 0xc0000 && a + bytes > 0xa0000) &&
           (!write || a + bytes <= 0xf0000);
}
static void registers(struct X86Registers *r, const uint8_t *p)
{
    uint32_t words[11];
    for (unsigned i = 0; i < 11; ++i)
        words[i] = be32(p + i * 4);
    memcpy(r, words, sizeof(words));
    for (unsigned i = 0; i < 6; ++i)
        r->segment[i] = ((unsigned)p[44 + i * 2] << 8) | p[45 + i * 2];
}
static struct X86BulkRequest request(const uint8_t *p)
{
    struct X86BulkRequest b;
    b.size        = be32(p);
    b.operation   = be32(p + 4);
    b.source      = be32(p + 8);
    b.destination = be32(p + 12);
    b.width       = be32(p + 16);
    b.count       = be32(p + 20);
    b.value       = be32(p + 24);
    b.stride      = (int32_t)be32(p + 28);
    if (b.size != sizeof(b) || b.operation > X86_BULK_COPY || (b.width != 1 && b.width != 2 && b.width != 4) ||
        !b.count || b.count > X86_BULK_MAX || b.stride != (int32_t)b.width ||
        (b.operation == X86_BULK_COPY && !ordinary(b.source, b.count * b.width, 0)))
        error("invalid bulk record");
    return b;
}
static uint32_t X86_CALLBACK memory(void *user, uint32_t a, uint32_t w, uint32_t op, uint32_t *value)
{
    (void)user;
    if (failed || (w != 1 && w != 2 && w != 4) || op > X86_OUT)
        return 1;
    ++accesses[op];
    ++widths[w == 1 ? 0 : w == 2 ? 1 : 2];
    if (op <= X86_FETCH && ordinary(a, w, op == X86_WRITE)) {
        if (op == X86_WRITE)
            for (unsigned i = 0; i < w; ++i)
                ram[a + i] = *value >> (8 * i);
        else
            *value = little(ram + a, w);
        return 0;
    }
    struct Event e = peek();
    if (failed)
        return 1;
    if (e.kind == PI_RECORD_ACCESS && e.bytes == sizeof(struct PIRecordAccess)) {
        if (be32(e.data) != a || be32(e.data + 4) != w || be32(e.data + 8) != op)
            error("access address/width/type");
        if (op == X86_WRITE || op == X86_OUT) {
            if ((*value & mask(w)) != (be32(e.data + 12) & mask(w)))
                error("write value");
        } else
            *value = be32(e.data + 12);
        uint32_t status = be32(e.data + 16);
        pop(e);
        return failed ? 1 : status;
    }
    if (e.kind == PI_RECORD_BULK && e.bytes == sizeof(struct X86BulkRequest)) {
        struct X86BulkRequest b = request(e.data);
        if (failed)
            return 1;
        uint32_t expected = b.operation == X86_BULK_FILL ? b.value : little(ram + b.source + bulk_index * b.stride, w);
        if (op != X86_WRITE || a != b.destination + bulk_index * b.stride || w != b.width ||
            (*value & mask(w)) != (expected & mask(w)))
            error("expanded bulk write");
        if (++bulk_index == b.count)
            pop(e);
        return failed;
    }
    error("expected hardware access");
    return 1;
}
static uint32_t X86_CALLBACK bulk(void *user, const struct X86BulkRequest *b)
{
    (void)user;
    if (failed || b->stride != (int32_t)b->width)
        return 0;
    uint32_t bytes = b->count * b->width;
    if (b->operation == X86_BULK_COPY && !ordinary(b->source, bytes, 0))
        return 0;
    if (ordinary(b->destination, bytes, 1)) {
        if (b->operation == X86_BULK_COPY) {
            if (b->source < b->destination + bytes && b->destination < b->source + bytes)
                return 0;
            memcpy(ram + b->destination, ram + b->source, bytes);
        } else
            for (unsigned i = 0; i < bytes; ++i)
                ram[b->destination + i] = b->value >> (8 * (i % b->width));
        return b->count;
    }
    struct Event e = peek();
    if (e.kind != PI_RECORD_BULK)
        return 0;
    if (e.bytes != sizeof(struct X86BulkRequest)) {
        error("bulk record size");
        return 0;
    }
    struct X86BulkRequest r = request(e.data);
    if (failed)
        return 0;
    if (r.operation != b->operation || r.destination + bulk_index * r.stride != b->destination || r.width != b->width ||
        r.stride != b->stride) {
        error("bulk request");
        return 0;
    }
    if (b->operation == X86_BULK_COPY) {
        if (b->source != r.source + bulk_index * r.stride) {
            error("bulk source");
            return 0;
        }
    } else if ((r.value & mask(b->width)) != (b->value & mask(b->width))) {
        error("bulk value");
        return 0;
    }
    uint32_t count = r.count - bulk_index;
    if (count > b->count)
        count = b->count;
    ++bulk_calls;
    bulk_elements += count;
    bulk_index += count;
    if (bulk_index == r.count)
        pop(e);
    return count;
}
static uint32_t X86_CALLBACK interrupt(void *user, uint32_t number, uint32_t fault, struct X86Registers *r)
{
    (void)user;
    struct Event e = peek();
    struct X86Registers before;
    if (e.kind != PI_RECORD_INTERRUPT || e.bytes != sizeof(struct PIRecordInterrupt)) {
        error("interrupt event");
        return 2;
    }
    registers(&before, e.data + 12);
    if (be32(e.data) != number || be32(e.data + 4) != fault || memcmp(&before, r, sizeof(before)))
        error("interrupt registers");
    registers(r, e.data + 12 + sizeof(before));
    uint32_t handled = be32(e.data + 8);
    pop(e);
    return failed ? 2 : handled;
}
static int profile(x86emu_t *cpu)
{
    uint32_t a = cpu->x86.R_CS_BASE + cpu->x86.R_EIP;
    if (a >= 0xc0000 && a < 0xd0000)
        ++profile_pc[a - 0xc0000];
    else
        ++profile_other;
    return original_check(cpu);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    FILE *file      = NULL;
    uint8_t *stream = NULL;
    long length;
    X86Context *cpu                = NULL;
    struct X86CodeRegion regions[] = {{0, 0xa0000, ram}, {0xc0000, 0x40000, ram + 0xc0000}};
    struct X86Config config        = {.size         = sizeof(config),
                                      .version      = X86EMU_ABI_VERSION,
                                      .access       = memory,
                                      .interrupt    = interrupt,
                                      .region_count = 2,
                                      .regions      = regions,
                                      .bulk         = bulk};
    struct X86Limits limits        = {sizeof(limits), 10000000, 30000000, 0, NULL};
    struct X86Registers r, expected;
    struct X86Result result = {.size = sizeof(result)};
    unsigned repeats        = argc > 2 ? atoi(argv[2]) : 1;
    if (argc < 2 || !repeats || repeats > 100 || !(file = fopen(argv[1], "rb")))
        return 20;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    if (length < 4 || length > 32 * 1024 * 1024 || !(stream = malloc(length)))
        return 20;
    if (fread(stream, 1, length, file) != (size_t)length || be32(stream) != PI_RECORD_MAGIC)
        return 20;
    fclose(file);
    end       = stream + length;
    profiling = argc > 3;
    if (profiling && REPLAY_JIT) {
        fputs("PC profiling requires X86_BACKEND=libx86emu\n", stderr);
        free(stream);
        return 20;
    }
    for (unsigned iteration = 0; iteration < repeats && !failed; ++iteration) {
        cursor = stream + 4;
        events = bulk_index = 0;
        struct Event e      = peek();
        if (e.kind != PI_RECORD_RAM || e.bytes != RAM_SIZE) {
            error("initial RAM");
            break;
        }
        memcpy(ram, e.data, RAM_SIZE);
        pop(e);
        e = peek();
        if (e.kind != PI_RECORD_REGS || e.bytes != sizeof(r)) {
            error("initial registers");
            break;
        }
        registers(&r, e.data);
        pop(e);
        cpu = xe_create(&config);
        if (!cpu)
            return 20;
        if (profiling) {
            struct LibX86State *s = xe_backend_data(cpu);
            original_check        = s->cpu.code_check;
            s->cpu.code_check     = profile;
        }
        if (xe_set(cpu, &r) || xe_run(cpu, &limits, &result) || xe_get(cpu, &r))
            error("execution stopped");
        e = peek();
        if (e.kind != PI_RECORD_FINAL_RAM || e.bytes != RAM_SIZE || memcmp(ram, e.data, RAM_SIZE))
            error("final RAM");
        if (!failed) {
            pop(e);
            e = peek();
            if (e.kind != PI_RECORD_FINAL_REGS || e.bytes != sizeof(r))
                error("final registers event");
        }
        if (!failed) {
            registers(&expected, e.data);
            if (memcmp(&expected, &r, sizeof(r)))
                error("final registers");
            pop(e);
            e = peek();
        }
        if (!failed) {
            if (e.kind != PI_RECORD_RESULT || e.bytes != sizeof(result))
                error("final result event");
            else
                for (unsigned i = 0; i < sizeof(result) / 4; ++i)
                    if (((uint32_t *)&result)[i] != be32(e.data + i * 4))
                        error("execution counts/stop state");
            pop(e);
            if (cursor != end)
                error("unconsumed events");
        }
        xe_delete(cpu);
        cpu = NULL;
    }
    if (profiling && !failed) {
        file = fopen(argv[3], "w");
        if (!file)
            return 20;
        for (unsigned i = 0; i < 65536; ++i)
            if (profile_pc[i])
                fprintf(file, "%05x %lu\n", 0xc0000 + i, (unsigned long)profile_pc[i]);
        fclose(file);
    }
    printf("REPLAY RESULT failures=%lu events=%lu instructions=%lu accesses=%lu bulk_calls=%lu bulk_elements=%lu\n",
           (unsigned long)failed, (unsigned long)events, (unsigned long)result.instructions,
           (unsigned long)result.accesses, (unsigned long)bulk_calls, (unsigned long)bulk_elements);
    printf("REPLAY callbacks read=%lu write=%lu fetch=%lu in=%lu out=%lu other_pc=%lu\n", (unsigned long)accesses[0],
           (unsigned long)accesses[1], (unsigned long)accesses[2], (unsigned long)accesses[3],
           (unsigned long)accesses[4], (unsigned long)profile_other);
    free(stream);
    return failed ? 20 : 0;
}
