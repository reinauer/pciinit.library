#ifndef AMIGA_CLIB_X86EMU_PROTOS_H
#define AMIGA_CLIB_X86EMU_PROTOS_H
#include <libraries/x86emu.h>
/* API declarations for SDK tools. GCC clients include proto/x86emu.h. */
X86Context *X86Create(const struct X86Config *config);
uint32_t X86Delete(X86Context *context);
uint32_t X86GetRegisters(X86Context *context, struct X86Registers *registers);
uint32_t X86SetRegisters(X86Context *context, const struct X86Registers *registers);
uint32_t X86Run(X86Context *context, const struct X86Limits *limits, struct X86Result *result);
uint32_t X86ContextBytes(void);
#endif
