/* Common library policy; the selected backend owns CPU state and execution. */
#include "private/backend.h"
#include "private/runtime.h"
#include <string.h>
#ifdef __amigaos__
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

static uint64_t now(X86Context *ctx)
{
#ifdef __amigaos__
    struct Library *TimerBase = (struct Library *)ctx->timer->tr_node.io_Device;
    struct EClockVal clock;
    ctx->hz = ReadEClock(&clock);
    return ((uint64_t)clock.ev_hi << 32) | clock.ev_lo;
#else
    struct timespec ts;
    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void xe_stop(X86Context *ctx, uint32_t reason)
{
    if (!ctx->stopped) {
        ctx->stopped       = 1;
        ctx->result.reason = reason;
    }
    xe_backend_stop(ctx);
}
int xe_expired(X86Context *ctx)
{
#ifdef __amigaos__
    struct ExecBase *SysBase = pi_sysbase();
    /* ReadEClock-only device models leave io_Unit unset. Retain the clock
     * path there, and while disabled interrupts prevent timer delivery. */
    if (ctx->timer->tr_node.io_Unit && SysBase->IDNestCnt < 0)
        return CheckIO((struct IORequest *)ctx->timer) != NULL;
#endif
    uint64_t elapsed = now(ctx) - ctx->started;
    return elapsed >= ctx->timeout_ticks;
}
void xe_fault(X86Context *ctx, uint32_t number)
{
    ctx->result.interrupt = number;
    xe_stop(ctx, X86_STOP_FAULT);
}
int xe_interrupt(X86Context *ctx, uint32_t number, uint32_t fault)
{
    uint32_t handled      = 0;
    struct X86Registers r = {.size = sizeof(r)};
    xe_backend_get(ctx, &r);
    if (ctx->config.interrupt)
        handled = ctx->config.interrupt(ctx->config.user, number, fault, &r);
    if (handled == 1) {
        xe_backend_set(ctx, &r, 0);
        return 1;
    }
    if (handled || fault) {
        xe_fault(ctx, number);
        return 1;
    }
    return 0;
}
uint32_t xe_context_bytes(void)
{
    uint32_t bytes = xe_backend_bytes();
    return bytes > UINT32_MAX - (uint32_t)offsetof(X86Context, backend)
               ? 0
               : (uint32_t)offsetof(X86Context, backend) + bytes;
}
X86Context *xe_create(const struct X86Config *config)
{
    X86Context *ctx;
    uint32_t bytes;
    uint32_t i, j;
    if (!config || !config->access || config->size != sizeof(*config) || config->version != X86EMU_ABI_VERSION)
        return NULL;
    if (config->region_count > X86_MAX_CODE_REGIONS || (config->region_count && !config->regions))
        return NULL;
    for (i = 0; i < config->region_count; ++i) {
        const struct X86CodeRegion *r = config->regions + i;
        if (!r->data || !r->bytes || r->address > UINT32_MAX - r->bytes)
            return NULL;
        for (j = 0; j < i; ++j) {
            const struct X86CodeRegion *p = config->regions + j;
            if (r->address < p->address + p->bytes && p->address < r->address + r->bytes)
                return NULL;
        }
    }
    if (config->bulk_elements > X86_SPAN_MAX || (config->data_regions >> config->region_count))
        return NULL;
    bytes = xe_context_bytes();
    if (!bytes || !(ctx = pi_alloc(bytes)))
        return NULL;
    ctx->config        = *config;
    ctx->bulk          = config->bulk;
    ctx->scan          = config->scan;
    ctx->data_regions  = config->data_regions;
    ctx->bulk_elements = config->bulk_elements ? config->bulk_elements : X86_BULK_MAX;
    ctx->region_count  = config->region_count;
    for (i = 0; i < ctx->region_count; ++i)
        ctx->regions[i] = config->regions[i];
    if (xe_backend_init(ctx)) {
        xe_backend_destroy(ctx);
        pi_free(ctx);
        return NULL;
    }
#ifdef __amigaos__
    {
        struct ExecBase *SysBase = pi_sysbase();
        ctx->port                = CreateMsgPort();
        if (ctx->port)
            ctx->timer = (struct timerequest *)CreateIORequest(ctx->port, sizeof(*ctx->timer));
        if (!ctx->timer || OpenDevice((STRPTR)TIMERNAME, UNIT_WAITECLOCK, (struct IORequest *)ctx->timer, 0)) {
            if (ctx->timer)
                DeleteIORequest((struct IORequest *)ctx->timer);
            if (ctx->port)
                DeleteMsgPort(ctx->port);
            xe_backend_destroy(ctx);
            pi_free(ctx);
            return NULL;
        }
        /* Completion is polled, never waited for during execution. Do not
         * send a stray signal to the task which created this context. */
        ctx->port->mp_Flags = PA_IGNORE;
        now(ctx);
        if (!ctx->hz) {
            xe_delete(ctx);
            return NULL;
        }
    }
#endif
    return ctx;
}
uint32_t xe_delete(X86Context *ctx)
{
    if (!ctx)
        return X86_STOP_BADARG;
    if (ctx->active)
        return X86_STOP_BUSY;
#ifdef __amigaos__
    {
        struct ExecBase *SysBase = pi_sysbase();
        CloseDevice((struct IORequest *)ctx->timer);
        DeleteIORequest((struct IORequest *)ctx->timer);
        DeleteMsgPort(ctx->port);
    }
#endif
    xe_backend_destroy(ctx);
    pi_free(ctx);
    return 0;
}

uint32_t xe_get(X86Context *ctx, struct X86Registers *r)
{
    if (!ctx || !r || r->size != sizeof(*r))
        return X86_STOP_BADARG;
    if (ctx->active)
        return X86_STOP_BUSY;
    xe_backend_get(ctx, r);
    return 0;
}
uint32_t xe_set(X86Context *ctx, const struct X86Registers *r)
{
    if (!ctx || !r || r->size != sizeof(*r))
        return X86_STOP_BADARG;
    if (ctx->active)
        return X86_STOP_BUSY;
    xe_backend_set(ctx, r, 1);
    return 0;
}
uint32_t xe_run(X86Context *ctx, const struct X86Limits *limits, struct X86Result *out)
{
    if (!ctx || !limits || limits->size != sizeof(*limits) || !out || out->size != sizeof(*out) ||
        !limits->instructions || !limits->accesses)
        return X86_STOP_BADARG;
    if (ctx->active)
        return X86_STOP_BUSY;
    ctx->active  = 1;
    ctx->stopped = 0;
    ctx->limits  = *limits;
    memset(&ctx->result, 0, sizeof(ctx->result));
    ctx->result.size = sizeof(ctx->result);
    ctx->started     = now(ctx);
#ifdef __amigaos__
    /* Round the absolute deadline up once. Timer completion handles the
     * ordinary polls; the same deadline backs the direct-clock fallback. */
    ctx->timeout_ticks = ((uint64_t)limits->milliseconds * ctx->hz + 999) / 1000;
    if (limits->milliseconds && ctx->timer->tr_node.io_Unit) {
        struct ExecBase *SysBase       = pi_sysbase();
        uint64_t deadline              = ctx->started + ctx->timeout_ticks;
        ctx->timer->tr_node.io_Command = TR_ADDREQUEST;
        ctx->timer->tr_time.tv_secs    = deadline >> 32;
        ctx->timer->tr_time.tv_micro   = deadline;
        SendIO((struct IORequest *)ctx->timer);
    }
#else
    ctx->timeout_ticks = limits->milliseconds;
#endif
    xe_backend_run(ctx);
#ifdef __amigaos__
    if (limits->milliseconds && ctx->timer->tr_node.io_Unit) {
        struct ExecBase *SysBase = pi_sysbase();
        if (!CheckIO((struct IORequest *)ctx->timer))
            AbortIO((struct IORequest *)ctx->timer);
        WaitIO((struct IORequest *)ctx->timer);
    }
#endif
    ctx->active = 0;
    *out        = ctx->result;
    return out->reason;
}
