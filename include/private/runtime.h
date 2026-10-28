#ifndef PCIINIT_RUNTIME_H
#define PCIINIT_RUNTIME_H
#include <stddef.h>
void *pi_alloc(size_t bytes);
void pi_free(void *memory);
#ifdef __amigaos__
struct ExecBase;
static inline struct ExecBase *pi_sysbase(void)
{
    struct ExecBase *base;
    __asm volatile("move.l 4.w,%0" : "=a"(base));
    return base;
}
#endif
#endif
