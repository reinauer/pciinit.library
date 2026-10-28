#include "private/library.h"
#include <libraries/x86emu.h>
#ifdef PCIINIT_LIBRARY
#include "private/version.h"
#endif
#include <proto/exec.h>

static void free_base(struct PIBase *base)
{
    struct ExecBase *SysBase = base->sys;
    ULONG negative           = base->library.lib_NegSize;
    FreeMem((UBYTE *)base - negative, negative + base->library.lib_PosSize);
}
APTR lib_open(BASE)
{
    ++base->library.lib_OpenCnt;
    base->library.lib_Flags &= ~LIBF_DELEXP;
    return base;
}
BPTR lib_expunge(BASE)
{
    struct ExecBase *SysBase = base->sys;
    BPTR segment;
    base->library.lib_Flags |= LIBF_DELEXP;
    if (base->library.lib_OpenCnt || base->objects)
        return 0;
    segment = base->segment;
    Remove(&base->library.lib_Node);
    if (base->engine)
        CloseLibrary(base->engine);
    free_base(base);
    return segment;
}
BPTR lib_close(BASE)
{
    if (base->library.lib_OpenCnt)
        --base->library.lib_OpenCnt;
    return base->library.lib_Flags & LIBF_DELEXP ? lib_expunge(base) : 0;
}
ULONG lib_reserved(void)
{
    return 0;
}
static APTR lib_init(REG(struct PIBase *, base, d0), REG(BPTR, segment, a0), REG(struct ExecBase *, SysBase, a6))
{
    base->sys     = SysBase;
    base->segment = segment;
    if (SysBase->LibNode.lib_Version < 39 || !(SysBase->AttnFlags & AFF_68020)) {
        free_base(base);
        return NULL;
    }
    base->library.lib_Node.ln_Type = NT_LIBRARY;
    base->library.lib_Node.ln_Name = (char *)lib_name;
    base->library.lib_Flags        = LIBF_CHANGED | LIBF_SUMUSED;
#ifdef PCIINIT_LIBRARY
    base->library.lib_Version  = PCIINIT_VERSION;
    base->library.lib_Revision = PCIINIT_REVISION;
#else
    base->library.lib_Version  = X86EMU_VERSION;
    base->library.lib_Revision = 0;
#endif
    base->library.lib_IdString = (STRPTR)lib_ident;
#ifdef PCIINIT_LIBRARY
    base->engine = OpenLibrary((STRPTR) "x86emu.library", X86EMU_VERSION);
    if (!base->engine) {
        free_base(base);
        return NULL;
    }
#endif
    return base;
}
const struct
{
    ULONG size;
    const APTR *vectors;
    APTR data, init;
} lib_autoinit = {sizeof(struct PIBase), lib_vectors, NULL, lib_init};
