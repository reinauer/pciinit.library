/* libx86emu backend. api.c/mem.c are deliberately not linked: context
 * allocation, execution limits and client callbacks belong to the frontend. */
#include "private/libx86emu_backend.h"
#include <string.h>
/* Both backend states begin with this complete interpreter state. */
static struct LibX86State *state(X86Context *ctx)
{
    return xe_backend_data(ctx);
}
static int instruction(x86emu_t *cpu)
{
    return xe_instruction(cpu->_private);
}
static unsigned access(x86emu_t *cpu, u32 address, u32 *value, unsigned type)
{
    uint32_t width = (type & 255) == X86EMU_MEMIO_8_NOPERM ? 1 : 1U << (type & 255);
    return xe_access(cpu->_private, address, width, type >> 8, value);
}
/* Width-specific instruction-fetch entry points retain the shared policy
 * while avoiding two indirect dispatches and runtime type/width decoding. */
unsigned x86emu_fetch8(x86emu_t *cpu, u32 address, u32 *value)
{
    return xe_access(cpu->_private, address, 1, X86_FETCH, value);
}
unsigned x86emu_fetch16(x86emu_t *cpu, u32 address, u32 *value)
{
    return xe_access(cpu->_private, address, 2, X86_FETCH, value);
}
unsigned x86emu_fetch32(x86emu_t *cpu, u32 address, u32 *value)
{
    return xe_access(cpu->_private, address, 4, X86_FETCH, value);
}
static int interrupt(x86emu_t *cpu, u8 number, unsigned type)
{
    return xe_interrupt(cpu->_private, number, (type & INTR_MODE_RESTART) || ((type & 255) == INTR_TYPE_FAULT));
}
static void unsupported_msr(x86emu_t *cpu)
{
    xe_stop(cpu->_private, X86_STOP_FAULT);
}
int xe_lib_init(X86Context *ctx)
{
    struct LibX86State *s = state(ctx);
    unsigned i;
    s->cpu.x86.msr      = s->msr;
    s->cpu.x86.msr_perm = s->msr_perm;
    s->cpu._private     = ctx;
    s->cpu.memio        = access;
    s->cpu.intr         = interrupt;
    s->cpu.code_check   = instruction;
    s->cpu.rdmsr = s->cpu.wrmsr = unsupported_msr;
    s->cpu.x86.R_EFLG           = 2;
    s->cpu.x86.R_IDT_LIMIT      = 0x3ff;
    for (i = 0; i < 6; ++i) {
        s->cpu.x86.seg[i].limit = 0xffff;
        s->cpu.x86.seg[i].acc   = i == X86_CS ? 0x9b : 0x93;
    }
    return 0;
}
void xe_lib_destroy(X86Context *ctx)
{
    (void)ctx;
}
void xe_lib_stop(X86Context *ctx)
{
    x86emu_stop(&state(ctx)->cpu);
}
void xe_lib_run(X86Context *ctx)
{
    x86emu_t *cpu = &state(ctx)->cpu;
    x86emu_run(cpu, 0);
    if (!ctx->stopped && cpu->x86.intr_type)
        xe_fault(ctx, cpu->x86.intr_nr);
}
void xe_lib_get(X86Context *ctx, struct X86Registers *r)
{
    x86emu_t *e = &state(ctx)->cpu;
    unsigned i;
    r->eax    = e->x86.R_EAX;
    r->ebx    = e->x86.R_EBX;
    r->ecx    = e->x86.R_ECX;
    r->edx    = e->x86.R_EDX;
    r->esi    = e->x86.R_ESI;
    r->edi    = e->x86.R_EDI;
    r->ebp    = e->x86.R_EBP;
    r->esp    = e->x86.R_ESP;
    r->eip    = e->x86.R_EIP;
    r->eflags = e->x86.R_EFLG;
    for (i = 0; i < 6; ++i)
        r->segment[i] = e->x86.seg[i].sel;
}
static void set_registers(X86Context *ctx, const struct X86Registers *r)
{
    x86emu_t *e = &state(ctx)->cpu;
    unsigned i;
    e->x86.R_EAX  = r->eax;
    e->x86.R_EBX  = r->ebx;
    e->x86.R_ECX  = r->ecx;
    e->x86.R_EDX  = r->edx;
    e->x86.R_ESI  = r->esi;
    e->x86.R_EDI  = r->edi;
    e->x86.R_EBP  = r->ebp;
    e->x86.R_ESP  = r->esp;
    e->x86.R_EIP  = r->eip;
    e->x86.R_EFLG = r->eflags | 2;
    for (i = 0; i < 6; ++i)
        if (e->x86.seg[i].sel != r->segment[i])
            x86emu_set_seg_register(e, e->x86.seg + i, r->segment[i]);
}

void xe_lib_set(X86Context *ctx, const struct X86Registers *r, int reset)
{
    x86emu_t *cpu = &state(ctx)->cpu;
    if (reset) {
        /* Public register writes start a real-mode call; RAM stays external. */
        unsigned i;
        cpu->x86.R_CR0 = 0;
        for (i = 0; i < 6; ++i) {
            cpu->x86.seg[i].base  = (uint32_t)r->segment[i] << 4;
            cpu->x86.seg[i].sel   = r->segment[i];
            cpu->x86.seg[i].limit = 0xffff;
            cpu->x86.seg[i].acc   = i == X86_CS ? 0x9b : 0x93;
        }
        cpu->x86.intr_type = 0;
    }
    set_registers(ctx, r);
}

/* Engine helper symbols from upstream api.c, without its allocator. */
unsigned x86emu_read_byte(x86emu_t *e, unsigned a)
{
    u32 v = 0;
    e->memio(e, a, &v, 0);
    return v;
}
unsigned x86emu_read_byte_noperm(x86emu_t *e, unsigned a)
{
    return x86emu_read_byte(e, a);
}
unsigned x86emu_read_word(x86emu_t *e, unsigned a)
{
    u32 v = 0;
    e->memio(e, a, &v, 1);
    return v;
}
unsigned x86emu_read_dword(x86emu_t *e, unsigned a)
{
    u32 v = 0;
    e->memio(e, a, &v, 2);
    return v;
}
void x86emu_write_byte(x86emu_t *e, unsigned a, unsigned v)
{
    u32 value = v;
    e->memio(e, a, &value, 0x100);
}
void x86emu_write_byte_noperm(x86emu_t *e, unsigned a, unsigned v)
{
    x86emu_write_byte(e, a, v);
}
void x86emu_write_word(x86emu_t *e, unsigned a, unsigned v)
{
    u32 value = v;
    e->memio(e, a, &value, 0x101);
}
void x86emu_write_dword(x86emu_t *e, unsigned a, unsigned v)
{
    u32 value = v;
    e->memio(e, a, &value, 0x102);
}
void x86emu_reset_access_stats(x86emu_t *e)
{
    memset(e->x86.intr_stats, 0, sizeof(e->x86.intr_stats));
}
unsigned x86emu_clear_log(x86emu_t *e, int flush)
{
    (void)e;
    (void)flush;
    return 0;
}
void x86emu_log(x86emu_t *e, const char *format, ...)
{
    (void)e;
    (void)format;
}
void x86emu_dump(x86emu_t *e, int flags)
{
    (void)e;
    (void)flags;
}

int xe_lib_step(X86Context *ctx)
{
    struct LibX86State *s = state(ctx);
    /* Retire one complete instruction, including interrupt delivery, but
     * avoid preparing and rejecting a second interpreter iteration. */
    x86emu_run(&s->cpu, X86EMU_RUN_SINGLE_STEP);
    if (!ctx->stopped && s->cpu.x86.intr_type)
        xe_fault(ctx, s->cpu.x86.intr_nr);
    return !!(s->cpu.x86.mode & _MODE_HALTED);
}

/* Number of elements before a segment, index or linear-address boundary.
 * The scalar helpers retain responsibility for any boundary fault. */
static uint32_t string_span(sel_t *seg, uint32_t offset, unsigned width, unsigned address32, int reverse,
                            uint32_t maximum)
{
    uint32_t limit = address32 ? UINT32_MAX : 0xffffU, room;
    if (seg->limit < limit)
        limit = seg->limit;
    if (UINT32_MAX - seg->base < limit)
        limit = UINT32_MAX - seg->base;
    if (offset > limit || width - 1 > limit - offset)
        return 0;
    if (reverse)
        room = offset / width;
    else
        room = (limit - offset - (width - 1)) / width;
    return room >= maximum ? maximum : room + 1;
}

/* The four optional vendor entry hooks share this adapter with both
 * backends. All decoding, fetch accounting and instruction retirement
 * still happen in the interpreter. No CPU layout crosses the public ABI. */
int x86emu_bulk_string(x86emu_t *emu, unsigned opcode)
{
    X86Context *ctx    = emu->_private;
    unsigned copy      = opcode == 0xa4 || opcode == 0xa5;
    unsigned width     = (opcode & 1) ? (MODE_DATA32 ? 4 : 2) : 1;
    unsigned address32 = !!MODE_ADDR32, reverse = !!ACCESS_FLAG(F_DF);
    sel_t *source = emu->x86.default_seg, *dest = &emu->x86.seg[R_ES_INDEX];
    uint32_t count, value = emu->x86.R_EAX, scalar_run = 0;
    int32_t stride = reverse ? -(int32_t)width : (int32_t)width;
    if (!ctx->bulk || !MODE_REP || (emu->x86.R_CR0 & 1) || (emu->x86.R_EFLG & F_TF))
        return 0;
    if (!source)
        source = &emu->x86.seg[(emu->x86.mode & _MODE_SEG_DS_SS) ? R_SS_INDEX : R_DS_INDEX];
    count = address32 ? emu->x86.R_ECX : emu->x86.R_CX;
    /* Match upstream's stop-state contract, including count cleared
     * before the first iteration and index advancement on a failed one. */
    if (address32)
        emu->x86.R_ECX = 0;
    else
        emu->x86.R_CX = 0;
    while (!MODE_HALTED && count) {
        uint32_t si  = address32 ? emu->x86.R_ESI : emu->x86.R_SI;
        uint32_t di  = address32 ? emu->x86.R_EDI : emu->x86.R_DI;
        unsigned per = copy ? 2 : 1;
        uint32_t n = string_span(dest, di, width, address32, reverse, ctx->bulk_elements), done = 0, room;
        if (copy) {
            room = string_span(source, si, width, address32, reverse, ctx->bulk_elements);
            if (n > room)
                n = room;
        }
        if (n > count)
            n = count;
        /* One poll per request; requests may cross polling boundaries, and
         * a decline is followed by scalar elements up to the next boundary
         * rather than by a retry. */
        if (scalar_run)
            n = 0;
        else if (n && xe_poll(ctx))
            break;
        room = (ctx->limits.accesses - ctx->result.accesses) / per;
        if (n > room)
            n = room;
        if (n) {
            struct X86BulkRequest request = {sizeof(request),
                                             copy ? X86_BULK_COPY : X86_BULK_FILL,
                                             copy ? source->base + si : 0,
                                             dest->base + di,
                                             width,
                                             n,
                                             value,
                                             stride};
            done                          = ctx->bulk(ctx->config.user, &request);
            if (done > n) {
                xe_stop(ctx, X86_STOP_BADARG);
                break;
            }
            ctx->result.accesses += done * per;
        }
        if (!done) {
            if (copy) {
                if (width == 1)
                    value = fetch_data_byte_abs(emu, source, si);
                else if (width == 2)
                    value = fetch_data_word_abs(emu, source, si);
                else
                    value = fetch_data_long_abs(emu, source, si);
            }
            if (width == 1)
                store_data_byte_abs(emu, dest, di, value);
            else if (width == 2)
                store_data_word_abs(emu, dest, di, value);
            else
                store_data_long_abs(emu, dest, di, value);
            done       = 1;
            scalar_run = (ctx->result.accesses & 255) != 0;
        }
        if (address32) {
            emu->x86.R_EDI += stride * done;
            if (copy)
                emu->x86.R_ESI += stride * done;
        } else {
            emu->x86.R_DI += stride * done;
            if (copy)
                emu->x86.R_SI += stride * done;
        }
        count -= done;
    }
    return 1;
}

/* SCAS keeps CX live, unlike upstream MOVS/STOS. A failed scalar read
 * still advances DI/CX and computes flags from its returned zero. */
int x86emu_bulk_scan(x86emu_t *emu, unsigned opcode)
{
    X86Context *ctx    = emu->_private;
    unsigned width     = (opcode & 1) ? (MODE_DATA32 ? 4 : 2) : 1;
    unsigned address32 = !!MODE_ADDR32, reverse = !!ACCESS_FLAG(F_DF);
    sel_t *seg        = &emu->x86.seg[R_ES_INDEX];
    uint32_t expected = width == 1 ? emu->x86.R_AL : width == 2 ? emu->x86.R_AX : emu->x86.R_EAX;
    int32_t stride    = reverse ? -(int32_t)width : (int32_t)width;
    if (!ctx->scan || !MODE_REP || (emu->x86.R_CR0 & 1) || (emu->x86.R_EFLG & F_TF))
        return 0;
    while (!MODE_HALTED && (address32 ? emu->x86.R_ECX : emu->x86.R_CX)) {
        uint32_t di    = address32 ? emu->x86.R_EDI : emu->x86.R_DI;
        uint32_t count = address32 ? emu->x86.R_ECX : emu->x86.R_CX;
        uint32_t n = string_span(seg, di, width, address32, reverse, X86_BULK_MAX), done = 0, value = 0, room;
        unsigned prepared = 0;
        if (n > count)
            n = count;
        if (n && (ctx->result.accesses & 255) == 255 && ctx->result.accesses < ctx->limits.accesses) {
            prepared = 1;
            if (xe_begin_access(ctx, &value))
                done = 1;
        }
        room = 255 - (ctx->result.accesses & 255) + prepared;
        if (n > room)
            n = room;
        room = ctx->limits.accesses - ctx->result.accesses + prepared;
        if (n > room)
            n = room;
        if (n && !done) {
            const struct X86ScanRequest request = {sizeof(request), seg->base + di, width, n,
                                                   expected,        !!MODE_REPE,    stride};
            done                                = ctx->scan(ctx->config.user, &request, &value);
            if (done > n) {
                xe_stop(ctx, X86_STOP_BADARG);
                break;
            }
            if (done)
                ctx->result.accesses += done - prepared;
        }
        if (!done) {
            if (prepared)
                xe_finish_access(ctx, seg->base + di, width, X86_READ, &value);
            else if (width == 1)
                value = fetch_data_byte_abs(emu, seg, di);
            else if (width == 2)
                value = fetch_data_word_abs(emu, seg, di);
            else
                value = fetch_data_long_abs(emu, seg, di);
            done = 1;
        }
        if (width == 1)
            cmp_byte(emu, expected, value);
        else if (width == 2)
            cmp_word(emu, expected, value);
        else
            cmp_long(emu, expected, value);
        if (address32) {
            emu->x86.R_EDI += stride * done;
            emu->x86.R_ECX -= done;
        } else {
            emu->x86.R_DI += stride * done;
            emu->x86.R_CX -= done;
        }
        if (!!ACCESS_FLAG(F_ZF) != !!MODE_REPE)
            break;
    }
    return 1;
}
