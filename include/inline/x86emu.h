#ifndef AMIGA_INLINE_X86EMU_H
#define AMIGA_INLINE_X86EMU_H
#include <exec/libraries.h>
#include <libraries/x86emu.h>
#define XREG(type, name, reg) type name __asm(#reg)
#define XVEC(offset, type)    ((type)((unsigned char *)X86EmuBase - (offset)))
static inline X86Context *X86Create(const struct X86Config *config)
{
    typedef X86Context *(*fn)(XREG(const struct X86Config *, c, a0), XREG(struct Library *, b, a6));
    return XVEC(30, fn)(config, X86EmuBase);
}
static inline uint32_t X86Delete(X86Context *ctx)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(struct Library *, b, a6));
    return XVEC(36, fn)(ctx, X86EmuBase);
}
static inline uint32_t X86GetRegisters(X86Context *ctx, struct X86Registers *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(struct X86Registers *, r, a1),
                           XREG(struct Library *, b, a6));
    return XVEC(42, fn)(ctx, r, X86EmuBase);
}
static inline uint32_t X86SetRegisters(X86Context *ctx, const struct X86Registers *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(const struct X86Registers *, r, a1),
                           XREG(struct Library *, b, a6));
    return XVEC(48, fn)(ctx, r, X86EmuBase);
}
static inline uint32_t X86Run(X86Context *ctx, const struct X86Limits *limits, struct X86Result *r)
{
    typedef uint32_t (*fn)(XREG(X86Context *, c, a0), XREG(const struct X86Limits *, l, a1),
                           XREG(struct X86Result *, r, a2), XREG(struct Library *, b, a6));
    return XVEC(54, fn)(ctx, limits, r, X86EmuBase);
}
static inline uint32_t X86ContextBytes(void)
{
    typedef uint32_t (*fn)(XREG(struct Library *, b, a6));
    return XVEC(60, fn)(X86EmuBase);
}
#endif
