#ifndef PCIINIT_BACKEND_H
#define PCIINIT_BACKEND_H
#include "private/engine.h"
#include "private/runtime.h"
#include <stddef.h>
#ifdef __amigaos__
#include <exec/execbase.h>
#include <exec/io.h>
#include <devices/timer.h>
#endif

/* Engine-independent execution policy. No upstream CPU types here. */
struct X86Context
{
    struct X86Config config;
    struct X86Limits limits;
    struct X86Result result;
    uint32_t active, stopped;
    uint64_t started, timeout_ticks;
    X86BulkFn bulk;
    X86ScanFn scan;
    uint32_t bulk_elements, data_regions;
    uint32_t region_count;
    struct X86CodeRegion regions[X86_MAX_CODE_REGIONS];
#ifdef __amigaos__
    struct MsgPort *port;
    struct timerequest *timer;
    uint32_t hz;
#endif
    /* Backend state shares the context allocation. Align it for uint64_t
     * and pointers; backends must not require stronger alignment. */
    union {
        uint64_t integer;
        void *pointer;
    } backend[];
};
static inline void *xe_backend_data(X86Context *ctx)
{
    return ctx->backend;
}
static inline const uint8_t *xe_code_pointer(X86Context *ctx, uint32_t address, uint32_t width)
{
    uint32_t i;
    for (i = 0; i < ctx->region_count; ++i) {
        const struct X86CodeRegion *r = &ctx->regions[i];
        if (address >= r->address && width <= r->bytes && address - r->address <= r->bytes - width)
            return r->data + (address - r->address);
    }
    return NULL;
}

/* One implementation is linked at build time. init failure must leave
 * state safe for destroy. set(reset=1) starts a new real-mode entry;
 * set(reset=0) applies interrupt-callback edits without resetting hidden
 * CPU state. The public register snapshot is not a full CPU checkpoint. */
uint32_t xe_backend_bytes(void);
int xe_backend_init(X86Context *ctx);
void xe_backend_destroy(X86Context *ctx);
void xe_backend_get(X86Context *ctx, struct X86Registers *registers);
void xe_backend_set(X86Context *ctx, const struct X86Registers *registers, int reset);
void xe_backend_run(X86Context *ctx);
void xe_backend_stop(X86Context *ctx);

void xe_stop(X86Context *ctx, uint32_t reason);
int xe_expired(X86Context *ctx);
int xe_interrupt(X86Context *ctx, uint32_t number, uint32_t fault);
void xe_fault(X86Context *ctx, uint32_t number);

/* The asynchronous deadline request completes through ReplyMsg. This is
 * the test CheckIO() performs, without the library call on every block;
 * the direct-clock cases stay in xe_expired(). */
static inline int xe_deadline(X86Context *ctx)
{
#ifdef __amigaos__
    struct IORequest *io = (struct IORequest *)ctx->timer;
    if (io->io_Unit && pi_sysbase()->IDNestCnt < 0)
        return (io->io_Flags & IOF_QUICK) || io->io_Message.mn_Node.ln_Type == NT_REPLYMSG;
#endif
    return xe_expired(ctx);
}
/* Inline the ordinary instruction/access path: a backend boundary must
 * not add a second indirect dispatch to every guest instruction/access. */
static inline int xe_poll(X86Context *ctx)
{
    if (ctx->stopped)
        return 1;
    if (ctx->limits.cancel && *ctx->limits.cancel)
        xe_stop(ctx, X86_STOP_CANCEL);
    if (ctx->limits.milliseconds && xe_deadline(ctx))
        xe_stop(ctx, X86_STOP_TIMEOUT);
    return ctx->stopped;
}
static inline int xe_instruction(X86Context *ctx)
{
    if (xe_poll(ctx))
        return 1;
    if (ctx->result.instructions >= ctx->limits.instructions) {
        xe_stop(ctx, X86_STOP_LIMIT);
        return 1;
    }
    ++ctx->result.instructions;
    return 0;
}
static inline unsigned xe_begin_access(X86Context *ctx, uint32_t *value)
{
    if (ctx->stopped) {
        *value = 0;
        return 1;
    }
    if (ctx->result.accesses >= ctx->limits.accesses) {
        xe_stop(ctx, X86_STOP_LIMIT);
        *value = 0;
        return 1;
    }
    ++ctx->result.accesses;
    if (!(ctx->result.accesses & 255) && xe_poll(ctx)) {
        *value = 0;
        return 1;
    }
    return 0;
}
/* 68020+ and the host test platforms permit unaligned word/long accesses. */
static inline uint32_t xe_load_le(const uint8_t *p, uint32_t width)
{
    if (width == 1)
        return p[0];
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (width == 2)
        return __builtin_bswap16(*(const uint16_t *)p);
    return __builtin_bswap32(*(const uint32_t *)p);
#else
    if (width == 2)
        return *(const uint16_t *)p;
    return *(const uint32_t *)p;
#endif
}
static inline void xe_store_le(uint8_t *p, uint32_t width, uint32_t v)
{
    if (width == 1) {
        p[0] = v;
        return;
    }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (width == 2)
        *(uint16_t *)p = __builtin_bswap16(v);
    else
        *(uint32_t *)p = __builtin_bswap32(v);
#else
    if (width == 2)
        *(uint16_t *)p = v;
    else
        *(uint32_t *)p = v;
#endif
}
static inline unsigned xe_finish_access(X86Context *ctx, uint32_t address, uint32_t width, uint32_t operation,
                                        uint32_t *value)
{
    if (operation == X86_FETCH) {
        const uint8_t *code = xe_code_pointer(ctx, address, width);
        if (code) {
            uint32_t i;
            *value = 0;
            for (i = 0; i < width; ++i)
                *value |= (uint32_t)code[i] << (8 * i);
            return 0;
        }
    } else if (operation <= X86_WRITE && ctx->data_regions) {
        /* Version-5 data regions are ordinary RAM: no callback, no side
         * effects, and the same little-endian layout as scalar access(). */
        uint32_t i;
        for (i = 0; i < ctx->region_count; ++i) {
            const struct X86CodeRegion *r = &ctx->regions[i];
            if (((ctx->data_regions >> i) & 1) && address >= r->address && width <= r->bytes &&
                address - r->address <= r->bytes - width) {
                uint8_t *p = (uint8_t *)r->data + (address - r->address);
                if (operation == X86_WRITE)
                    xe_store_le(p, width, *value);
                else
                    *value = xe_load_le(p, width);
                return 0;
            }
        }
    }
    if (ctx->config.access(ctx->config.user, address, width, operation, value)) {
        ctx->result.address   = address;
        ctx->result.operation = operation;
        xe_stop(ctx, X86_STOP_ACCESS);
        *value = 0;
        return 1;
    }
    return 0;
}
static inline unsigned xe_access(X86Context *ctx, uint32_t address, uint32_t width, uint32_t operation, uint32_t *value)
{
    return xe_begin_access(ctx, value) || xe_finish_access(ctx, address, width, operation, value);
}
#endif
