#ifndef PCIINIT_ENGINE_H
#define PCIINIT_ENGINE_H
#include <libraries/x86emu.h>
X86Context *xe_create(const struct X86Config *config);
uint32_t xe_delete(X86Context *context);
uint32_t xe_get(X86Context *context, struct X86Registers *registers);
uint32_t xe_set(X86Context *context, const struct X86Registers *registers);
uint32_t xe_run(X86Context *context, const struct X86Limits *limits, struct X86Result *result);
uint32_t xe_context_bytes(void);
#endif
