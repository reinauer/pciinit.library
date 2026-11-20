#ifndef PCIINIT_LIBRARY_BASE_H
#define PCIINIT_LIBRARY_BASE_H
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#define REG(type, name, reg) type name __asm(#reg)
struct PIBase
{
    struct Library library;
    struct ExecBase *sys;
    BPTR segment;
    ULONG objects;
    struct Library *engine;
    APTR mapping_owner;
    /* pciinit.library's VGA arbiter: held around every call that runs a
     * ROM on a card and between ObtainLegacyVGA and ReleaseLegacyVGA. */
    struct SignalSemaphore vga_lock;
    APTR vga_holder; /* the context whose card was last given legacy VGA */
};
#define BASE REG(struct PIBase *, base, a6)
APTR lib_open(BASE);
BPTR lib_close(BASE);
BPTR lib_expunge(BASE);
ULONG lib_reserved(void);
extern const char lib_name[], lib_ident[];
extern const APTR lib_vectors[];
#endif
