/* Bounded real-mode basic-block translator, selected at link time.
 * Memory, interrupts and unsupported instructions use the interpreter
 * adapter with its complete CPU state. No vendor decode-loop hooks. */
#include "private/libx86emu_backend.h"
#include "private/m68kjit.h"
#include "private/runtime.h"
#include <string.h>
#ifdef __amigaos__
#include <proto/exec.h>
#endif

#define BLOCKS     1024
#define INSNS      16
#define CODE_BYTES 128
/* A 1 KiB block makes the slot index a single shift. Sixteen translated
 * instructions need at most about 300 native words. */
#define NATIVE_WORDS 420
#ifdef X86EMU_NO_JIT_STATS
#define STAT(expression) ((void)0)
#else
#define STAT(expression) (expression)
#endif
struct Block
{
    uint32_t base, ip;
    const uint8_t *source;
    unsigned count, bytes, fetches, words;
    unsigned io_width, io_operation, io_port, io_dx;
    unsigned handler, handler_mode, handler_seg;
    uint8_t original[CODE_BYTES];
    uint16_t native[NATIVE_WORDS];
};
struct JitState
{
    struct LibX86State lib; /* Must remain first: shared fallback adapter. */
    struct X86JitStats stats;
    struct Block block[BLOCKS];
};
#ifdef __mc68000__
typedef char block_is_one_kilobyte[sizeof(struct Block) == 1024 ? 1 : -1];
#endif
static struct JitState *state(X86Context *ctx)
{
    return xe_backend_data(ctx);
}
uint32_t xe_backend_bytes(void)
{
    return sizeof(struct JitState);
}
int xe_backend_init(X86Context *ctx)
{
    return xe_lib_init(ctx);
}
void xe_backend_destroy(X86Context *ctx)
{
    xe_lib_destroy(ctx);
}
void xe_backend_stop(X86Context *ctx)
{
    xe_lib_stop(ctx);
}
void xe_backend_get(X86Context *ctx, struct X86Registers *r)
{
    xe_lib_get(ctx, r);
}
void xe_backend_set(X86Context *ctx, const struct X86Registers *r, int reset)
{
    xe_lib_set(ctx, r, reset);
}
void xe_jit_stats(X86Context *ctx, struct X86JitStats *stats)
{
    *stats = state(ctx)->stats;
}

#ifdef __mc68000__
#define OFF(field) ((unsigned)offsetof(x86emu_t, x86.field))
static const unsigned offsets[3][8] = {
    {OFF(R_AL), OFF(R_CL), OFF(R_DL), OFF(R_BL), OFF(R_AH), OFF(R_CH), OFF(R_DH), OFF(R_BH)},
    {OFF(R_AX), OFF(R_CX), OFF(R_DX), OFF(R_BX), OFF(R_SP), OFF(R_BP), OFF(R_SI), OFF(R_DI)},
    {OFF(R_EAX), OFF(R_ECX), OFF(R_EDX), OFF(R_EBX), OFF(R_ESP), OFF(R_EBP), OFF(R_ESI), OFF(R_EDI)}};
/* Helpers only touch registers/flags; no callback or fault can occur here. */
#define ALU_ROW(n) {(uintptr_t)n##_byte, (uintptr_t)n##_word, (uintptr_t)n##_long}
static const uintptr_t alu[11][3]  = {ALU_ROW(add),  ALU_ROW(or),  ALU_ROW(adc), ALU_ROW(sbb),
                                      ALU_ROW(and),  ALU_ROW(sub), ALU_ROW(xor), ALU_ROW(cmp),
                                      ALU_ROW(test), ALU_ROW(inc), ALU_ROW(dec)};
static const uintptr_t shift[8][3] = {ALU_ROW(rol), ALU_ROW(ror), ALU_ROW(rcl), ALU_ROW(rcr),
                                      ALU_ROW(shl), ALU_ROW(shr), ALU_ROW(shl), ALU_ROW(sar)};
static unsigned loop_condition(x86emu_t *cpu, unsigned op)
{
    if (op == 0xe3)
        return cpu->x86.R_CX == 0;
    --cpu->x86.R_CX;
    return cpu->x86.R_CX && (op == 0xe2 || (!!(cpu->x86.R_EFLG & F_ZF) == (op == 0xe1)));
}
static void word(struct Block *b, unsigned w)
{
    b->native[b->words++] = w;
}
static void longword(struct Block *b, uint32_t v)
{
    word(b, v >> 16);
    word(b, v);
}
static void immediate(struct Block *b, unsigned size, unsigned offset, uint32_t v)
{
    static const unsigned op[] = {0x157c, 0x357c, 0x257c};
    word(b, op[size]);
    if (size == 2)
        longword(b, v);
    else
        word(b, size == 0 ? v & 255 : v);
    word(b, offset);
}
static void load(struct Block *b, unsigned size, unsigned reg, unsigned dn)
{
    static const unsigned op[] = {0x102a, 0x302a, 0x202a};
    if (size != 2)
        word(b, 0x7000 + (dn << 9));
    word(b, op[size] + (dn << 9));
    word(b, offsets[size][reg]);
}
static void save(struct Block *b, unsigned size, unsigned reg, unsigned dn)
{
    static const unsigned op[] = {0x1540, 0x3540, 0x2540};
    word(b, op[size] + dn);
    word(b, offsets[size][reg]);
}
static void push_imm(struct Block *b, uint32_t v)
{
    word(b, 0x2f3c);
    longword(b, v);
}
static void call(struct Block *b, uintptr_t fn, unsigned args)
{
#ifdef X86EMU_JIT_REGPARM2
    /* The emitter has pushed the operands right-to-left. GCC's two-register
     * convention takes them in D0/D1 and the CPU pointer in A0. */
    word(b, 0x201f); /* move.l (sp)+,d0 */
    if (args == 3)
        word(b, 0x221f); /* move.l (sp)+,d1 */
    word(b, 0x204a);     /* move.l a2,a0 */
    word(b, 0x4eb9);
    longword(b, (uint32_t)fn);
#else
    word(b, 0x2f0a); /* CPU pointer */
    word(b, 0x4eb9);
    longword(b, (uint32_t)fn);
    word(b, 0x4fef);
    word(b, args * 4);
#endif
}
struct Decode
{
    X86Context *ctx;
    uint32_t address, ip;
    unsigned bytes, fetches, bad;
};
static uint32_t fetch(struct Decode *d, unsigned width)
{
    const uint8_t *p;
    uint32_t v = 0;
    unsigned i;
    if (d->bad || d->bytes + width > 15 || d->ip + d->bytes + width > 0xffff ||
        !(p = xe_code_pointer(d->ctx, d->address + d->bytes, width))) {
        d->bad = 1;
        return 0;
    }
    for (i = 0; i < width; ++i)
        v |= (uint32_t)p[i] << (8 * i);
    d->bytes += width;
    ++d->fetches;
    return v;
}
/* Return 0 for fallback, 1 for straight-line, 2 for a block terminator.
 * All emits are discarded on failure; no guest state changes at compile. */
static unsigned translate(struct Decode *d, struct Block *b)
{
    unsigned op = fetch(d, 1), size = 1, prefix = 0, mod, reg, src, kind = 0, imm = 0;
    uint32_t value   = 0;
    uintptr_t helper = 0;
    if (op == 0x66) {
        size   = 2;
        prefix = 1;
        op     = fetch(d, 1);
    }
    if (op >= 0xb0 && op <= 0xbf) {
        if (op < 0xb8)
            size = 0;
        value = fetch(d, 1U << size);
        immediate(b, size, offsets[size][op & 7], value);
        return 1;
    }
    if (op >= 0x90 && op <= 0x97) {
        reg = op & 7;
        if (reg) {
            load(b, size, 0, 0);
            load(b, size, reg, 1);
            save(b, size, 0, 1);
            save(b, size, reg, 0);
        }
        return 1;
    }
    if (op == 0x86 || op == 0x87) {
        if (!(op & 1))
            size = 0;
        mod = fetch(d, 1);
        if (mod < 0xc0)
            return 0;
        reg = mod & 7;
        src = (mod >> 3) & 7;
        load(b, size, reg, 0);
        load(b, size, src, 1);
        save(b, size, reg, 1);
        save(b, size, src, 0);
        return 1;
    }
    if ((op >= 0xe4 && op <= 0xe7) || (op >= 0xec && op <= 0xef)) {
        b->io_width     = (op & 1) ? 1U << size : 1;
        b->io_operation = (op & 2) ? X86_OUT : X86_IN;
        b->io_dx        = !!(op & 8);
        b->io_port      = b->io_dx ? 0 : fetch(d, 1);
        immediate(b, 1, OFF(R_IP), d->ip + d->bytes);
        /* End before calling client code: it can change mapped code or
         * request a stop. The runner accounts for fetches first. */
        return 2;
    }
    if (op >= 0x88 && op <= 0x8b) {
        if (!(op & 1))
            size = 0;
        mod = fetch(d, 1);
        if (mod < 0xc0)
            return 0;
        reg = mod & 7;
        src = (mod >> 3) & 7;
        if (op & 2) {
            unsigned tmp = reg;
            reg          = src;
            src          = tmp;
        }
        load(b, size, src, 0);
        save(b, size, reg, 0);
        return 1;
    }
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0xe0 && op <= 0xe3) || op == 0xeb) {
        uint16_t target;
        if (prefix)
            return 0;
        value  = fetch(d, 1);
        target = d->ip + d->bytes + (int8_t)value;
        immediate(b, 1, OFF(R_IP), d->ip + d->bytes);
        if (op < 0x80 && ((op & 15) < 6 || ((op & 15) >= 8 && (op & 15) < 12))) {
            static const unsigned bits[] = {11, 0, 6, 0, 7, 2};
            unsigned bit                 = bits[(op & 15) >> 1];
            word(b, 0x082a);
            word(b, bit & 7);
            word(b, OFF(R_EFLG) + 3 - bit / 8);
            word(b, (op & 1) ? 0x6606 : 0x6706); /* skip target if condition false */
        } else if (op != 0xeb) {
            push_imm(b, op < 0x80 ? op & 15 : op);
            call(b, op < 0x80 ? (uintptr_t)eval_condition : (uintptr_t)loop_condition, 2);
            word(b, 0x4a80);
            word(b, 0x6706); /* tst.l d0; beq.s +6 */
        }
        immediate(b, 1, OFF(R_IP), target);
        return 2;
    }
    if (op >= 0x40 && op <= 0x4f) {
        reg  = op & 7;
        kind = op < 0x48 ? 9 : 10;
    } else if (op < 0x40 && (op & 7) <= 5) {
        kind = op >> 3;
        if (!(op & 1))
            size = 0;
        if ((op & 7) >= 4) {
            reg   = 0;
            imm   = 1;
            value = fetch(d, 1U << size);
        } else {
            mod = fetch(d, 1);
            if (mod < 0xc0)
                return 0;
            reg = mod & 7;
            src = (mod >> 3) & 7;
            if (op & 2) {
                unsigned tmp = reg;
                reg          = src;
                src          = tmp;
            }
            load(b, size, src, 0);
        }
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        if (op == 0x80)
            size = 0;
        mod = fetch(d, 1);
        if (mod < 0xc0)
            return 0;
        kind  = (mod >> 3) & 7;
        reg   = mod & 7;
        imm   = 1;
        value = fetch(d, op == 0x83 ? 1 : 1U << size);
        if (op == 0x83)
            value = (int32_t)(int8_t)value;
    } else if (op == 0x84 || op == 0x85 || op == 0xa8 || op == 0xa9) {
        if (!(op & 1))
            size = 0;
        kind = 8;
        if (op >= 0xa8) {
            reg   = 0;
            imm   = 1;
            value = fetch(d, 1U << size);
        } else {
            mod = fetch(d, 1);
            if (mod < 0xc0)
                return 0;
            reg = mod & 7;
            load(b, size, (mod >> 3) & 7, 0);
        }
    } else if (op == 0xd0 || op == 0xd1 || op == 0xc0 || op == 0xc1) {
        if (!(op & 1))
            size = 0;
        mod = fetch(d, 1);
        if (mod < 0xc0)
            return 0;
        reg    = mod & 7;
        helper = shift[(mod >> 3) & 7][size];
        imm    = 1;
        value  = op < 0xd0 ? fetch(d, 1) : 1;
    } else
        return 0;
    if (kind < 9) {
        if (imm)
            push_imm(b, value);
        else
            word(b, 0x2f00);
    }
    load(b, size, reg, 0);
    word(b, 0x2f00);
    call(b, helper ? helper : alu[kind][size], kind < 9 ? 3 : 2);
    if (kind != 7 && kind != 8)
        save(b, size, reg, 0);
    return 1;
}
/* Cache prefix decoding for instructions which still use upstream's
 * semantic handler. Operands are fetched by that handler at execution;
 * only side-effect-free mapped opcode bytes are cached. */
static int cache_handler(X86Context *ctx, struct Block *b, uint32_t base, uint32_t ip)
{
#if defined(X86EMU_DIRECT_FETCH) && defined(X86EMU_NO_DISASSEMBLY) && defined(X86EMU_NO_TRACE_FORMATTING)
    struct Decode d = {ctx, base + ip, ip, 0, 0, 0};
    unsigned mode = 0, seg = 0, op;
    for (;;) {
        op = fetch(&d, 1);
        if (d.bad)
            return 0;
        switch (op) {
        case 0x26:
            seg = R_ES_INDEX + 1;
            break;
        case 0x2e:
            seg = R_CS_INDEX + 1;
            break;
        case 0x36:
            seg = R_SS_INDEX + 1;
            break;
        case 0x3e:
            seg = R_DS_INDEX + 1;
            break;
        case 0x64:
            seg = R_FS_INDEX + 1;
            break;
        case 0x65:
            seg = R_GS_INDEX + 1;
            break;
        case 0x66:
            mode ^= _MODE_DATA32;
            break;
        case 0x67:
            mode ^= _MODE_ADDR32;
            break;
        case 0xf0:
            break;
        case 0xf2:
            mode |= _MODE_REPNE;
            break;
        case 0xf3:
            mode |= _MODE_REPE;
            break;
        default:
            b->handler      = op + 1;
            b->handler_mode = mode;
            b->handler_seg  = seg;
            b->count        = 1;
            b->bytes        = d.bytes;
            b->fetches      = d.fetches;
            b->words        = 0;
            b->source       = xe_code_pointer(ctx, base + ip, b->bytes);
            memcpy(b->original, b->source, b->bytes);
            return 1;
        }
    }
#else
    (void)ctx;
    (void)b;
    (void)base;
    (void)ip;
    return 0;
#endif
}
static void compile(X86Context *ctx, struct Block *b, uint32_t base, uint32_t ip)
{
    unsigned end = 0;
    b->count = b->bytes = b->fetches = b->words = b->io_width = b->handler = 0;
    b->source                                                              = NULL;
    b->base                                                                = base;
    b->ip                                                                  = ip;
    word(b, 0x2f0a);
    word(b, 0x246f);
    word(b, 8); /* save A2; argument -> A2 */
    while (b->count < INSNS && b->bytes < CODE_BYTES - 15 && b->words < NATIVE_WORDS - 64) {
        struct Decode d = {ctx, base + ip + b->bytes, ip + b->bytes, 0, 0, 0};
        unsigned words  = b->words;
        end             = translate(&d, b);
        if (!end || d.bad) {
            b->words    = words;
            b->io_width = 0;
            end         = 0;
            if (!b->count) {
                const uint8_t *code = xe_code_pointer(ctx, base + ip, d.bytes ? d.bytes : 1);
                if (code) {
                    b->source = code;
                    b->bytes  = d.bytes ? d.bytes : 1;
                    memcpy(b->original, code, b->bytes);
                }
            }
            break;
        }
        b->bytes += d.bytes;
        b->fetches += d.fetches;
        ++b->count;
        if (end == 2)
            break;
    }
    if (!b->count) {
        cache_handler(ctx, b, base, ip);
        return;
    }
    if (end != 2)
        immediate(b, 1, OFF(R_IP), ip + b->bytes);
    word(b, 0x245f);
    word(b, 0x4e75); /* restore A2; rts */
    {
        const uint8_t *code = xe_code_pointer(ctx, base + ip, b->bytes);
        if (!code) {
            b->count = 0;
            return;
        }
        b->source = code;
        memcpy(b->original, code, b->bytes);
    }
    {
        struct ExecBase *SysBase = pi_sysbase();
        CacheClearU();
    }
    STAT(++state(ctx)->stats.compiled);
}
/* Region descriptors and their host addresses are immutable for the
 * context lifetime. Revalidate every cached byte, using ordinary RAM
 * loads; there is no device access or speculative overread here. */
static int matches(const struct Block *b)
{
    const uint8_t *a = b->source, *v = b->original;
    unsigned n = b->bytes;
    while (n >= 4) {
        if (*(const uint32_t *)a != *(const uint32_t *)v)
            return 0;
        a += 4;
        v += 4;
        n -= 4;
    }
    if (n >= 2) {
        if (*(const uint16_t *)a != *(const uint16_t *)v)
            return 0;
        a += 2;
        v += 2;
        n -= 2;
    }
    return !n || *a == *v;
}
#endif
void xe_backend_run(X86Context *ctx)
{
#ifdef __mc68000__
    struct JitState *s = state(ctx);
    x86emu_t *cpu      = &s->lib.cpu;
    if (!ctx->region_count) {
        xe_lib_run(ctx);
        return;
    }
    unsigned native = 0; /* the previous block ran translated code only */
    cpu->x86.mode &= ~_MODE_HALTED;
    while (!xe_poll(ctx)) {
        uint32_t ip = cpu->x86.R_EIP, base = cpu->x86.R_CS_BASE;
        /* Translated instructions cannot change CR0, CS, TF or raise an
         * interrupt, and they store a 16-bit IP, so only a semantic
         * handler or an interpreter step requires the mode recheck. */
        if (native || (!(cpu->x86.R_CR0 & 1) && !ACC_D(cpu->x86.R_CS_ACC) && !(cpu->x86.R_EFLG & F_TF) &&
                       !cpu->x86.intr_type && ip <= 0xffff && base <= UINT32_MAX - 0x10000)) {
            struct Block *b = &s->block[(ip ^ (base >> 4)) & (BLOCKS - 1)];
            if (b->bytes && b->base == base && b->ip == ip) {
                if (!matches(b)) {
                    b->bytes = b->count = 0;
                    STAT(++s->stats.invalidated);
                    compile(ctx, b, base, ip);
                }
            } else
                compile(ctx, b, base, ip);
            if (b->count && b->count <= ctx->limits.instructions - ctx->result.instructions &&
                b->fetches <= ctx->limits.accesses - ctx->result.accesses) {
                native = !b->handler;
                if (native) {
                    /* Generated block entries retain the explicit stack ABI. */
                    typedef void __attribute__((stkparm)) (*NativeBlock)(x86emu_t *);
                    ((NativeBlock)(void *)b->native)(cpu);
                }
                ctx->result.instructions += b->count;
                ctx->result.accesses += b->fetches;
#if defined(X86EMU_DIRECT_FETCH) && defined(X86EMU_NO_DISASSEMBLY) && defined(X86EMU_NO_TRACE_FORMATTING)
                if (b->handler) {
                    cpu->x86.mode        = b->handler_mode | (ACC_D(cpu->x86.R_SS_ACC) ? _MODE_STACK32 : 0);
                    cpu->x86.default_seg = b->handler_seg ? cpu->x86.seg + b->handler_seg - 1 : NULL;
                    cpu->x86.saved_cs    = cpu->x86.R_CS;
                    cpu->x86.saved_eip   = ip;
                    cpu->x86.instr_len   = b->bytes;
                    cpu->x86.R_EIP       = ip + b->bytes;
                    x86emu_optab[b->handler - 1](cpu, b->handler - 1);
                    if (cpu->x86.intr_type)
                        x86emu_handle_pending(cpu);
                    ++cpu->x86.R_TSC;
                    STAT(++s->stats.fallback);
                    if (cpu->x86.mode & _MODE_HALTED)
                        break;
                    continue;
                }
#endif
                cpu->x86.R_TSC += b->count;
                STAT(++s->stats.blocks);
                STAT(s->stats.instructions += b->count);
                if (b->io_width) {
                    uint32_t value = b->io_width == 1   ? cpu->x86.R_AL
                                     : b->io_width == 2 ? cpu->x86.R_AX
                                                        : cpu->x86.R_EAX;
                    xe_access(ctx, b->io_dx ? cpu->x86.R_DX : b->io_port, b->io_width, b->io_operation, &value);
                    if (b->io_operation == X86_IN) {
                        if (b->io_width == 1)
                            cpu->x86.R_AL = value;
                        else if (b->io_width == 2)
                            cpu->x86.R_AX = value;
                        else
                            cpu->x86.R_EAX = value;
                    }
                }
                continue;
            }
        }
        STAT(++s->stats.fallback);
        native = 0;
        if (xe_lib_step(ctx))
            break;
    }
#else
    /* Hosts validate the shared policy; native execution is tested on m68k. */
    xe_lib_run(ctx);
#endif
}
