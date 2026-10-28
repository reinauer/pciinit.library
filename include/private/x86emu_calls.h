#ifndef PCIINIT_X86EMU_CALLS_H
#define PCIINIT_X86EMU_CALLS_H
#include <exec/libraries.h>
#include <libraries/x86emu.h>
#define XREG(type, name, reg) type name __asm(#reg)
#define XVEC(offset, type)    ((type)((unsigned char *)base - (offset)))
static inline X86Context *x86_create(struct Library *base, const struct X86Config *config)
{
    typedef X86Context *(*fn)(XREG(const struct X86Config *, c, a0), XREG(struct Library *, b, a6));
    return XVEC(30, fn)(config, base);
}
static inline uint32_t x86_delete(struct Library *base, X86Context *ctx)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(struct Library *, b, a6));
    return XVEC(36, fn)(ctx, base);
}
static inline uint32_t x86_get(struct Library *base, X86Context *ctx, struct X86Registers *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(struct X86Registers *, r, a1),
                           XREG(struct Library *, b, a6));
    return XVEC(42, fn)(ctx, r, base);
}
static inline uint32_t x86_set(struct Library *base, X86Context *ctx, const struct X86Registers *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(const struct X86Registers *, r, a1),
                           XREG(struct Library *, b, a6));
    return XVEC(48, fn)(ctx, r, base);
}
static inline uint32_t x86_run(struct Library *base, X86Context *ctx, const struct X86Limits *limits,
                               struct X86Result *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(const struct X86Limits *, l, a1),
                           XREG(struct X86Result *, r, a2), XREG(struct Library *, b, a6));
    return XVEC(54, fn)(ctx, limits, r, base);
}
#undef XVEC
#undef XREG
#endif
