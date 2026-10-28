/* The reference interpreter backend has no translator dependency. */
#include "private/libx86emu_backend.h"
uint32_t xe_backend_bytes(void)
{
    return sizeof(struct LibX86State);
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
void xe_backend_run(X86Context *ctx)
{
    xe_lib_run(ctx);
}
void xe_backend_get(X86Context *ctx, struct X86Registers *r)
{
    xe_lib_get(ctx, r);
}
void xe_backend_set(X86Context *ctx, const struct X86Registers *r, int reset)
{
    xe_lib_set(ctx, r, reset);
}
