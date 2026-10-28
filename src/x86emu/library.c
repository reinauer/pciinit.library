#include "private/library.h"
#include "private/engine.h"
const char lib_name[] = "x86emu.library";
#ifdef X86EMU_INTERPRETER_ONLY
const char lib_ident[] = "x86emu.library 5.0 (28.10.2026) no JIT";
#else
const char lib_ident[] = "x86emu.library 5.0 (28.10.2026)";
#endif
static X86Context *create(REG(const struct X86Config *, config, a0), BASE)
{
    X86Context *ctx = xe_create(config);
    if (ctx)
        ++base->objects;
    return ctx;
}
static ULONG destroy(REG(X86Context *, ctx, a0), BASE)
{
    ULONG result = xe_delete(ctx);
    if (!result)
        --base->objects;
    return result;
}
static ULONG get(REG(X86Context *, ctx, a0), REG(struct X86Registers *, r, a1))
{
    return xe_get(ctx, r);
}
static ULONG set(REG(X86Context *, ctx, a0), REG(const struct X86Registers *, r, a1))
{
    return xe_set(ctx, r);
}
static ULONG run(REG(X86Context *, ctx, a0), REG(const struct X86Limits *, limits, a1),
                 REG(struct X86Result *, result, a2))
{
    return xe_run(ctx, limits, result);
}
static ULONG bytes(void)
{
    return xe_context_bytes();
}
const APTR lib_vectors[] = {lib_open, lib_close, lib_expunge, lib_reserved, create,  destroy,
                            get,      set,       run,         bytes,        (APTR)-1};
