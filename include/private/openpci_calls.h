#ifndef PCIINIT_OPENPCI_CALLS_H
#define PCIINIT_OPENPCI_CALLS_H

/* Register ABI from the public openpci.fd. No provider-private layout. */
#include <exec/libraries.h>
#include <proto/exec.h>
#include <libraries/openpci.h>
#include <libraries/pcitags.h>
#include <utility/tagitem.h>

/* Old libraries can open successfully without the newer region vectors. */
static inline struct Library *op_open(struct ExecBase *SysBase)
{
    struct Library *base = OpenLibrary((STRPTR) "openpci.library", 14);
    if (base && base->lib_NegSize < 252) {
        CloseLibrary(base);
        return NULL;
    }
    return base;
}

#define OREG(type, name, reg)     type name __asm(#reg)
#define OCALL(base, offset, type) ((type)((UBYTE *)(base) - (offset)))

static inline ULONG op_bus(struct Library *b)
{
    typedef ULONG (*fn)(OREG(struct Library *, b, a6));
    return OCALL(b, 30, fn)(b);
}

static inline struct pci_dev *op_find(struct Library *b, struct pci_dev *p)
{
    typedef struct pci_dev *(*fn)(OREG(struct pci_dev *, p, a0), OREG(struct TagItem *, tags, a1),
                                  OREG(struct Library *, b, a6));
    struct TagItem tags[] = {{TAG_DONE, 0}};
    return OCALL(b, 204, fn)(p, tags, b);
}
static inline ULONG op_attrs(struct Library *b, struct pci_dev *p, struct TagItem *tags)
{
    typedef ULONG (*fn)(OREG(struct pci_dev *, p, a0), OREG(struct TagItem *, tags, a1), OREG(struct Library *, b, a6));
    return OCALL(b, 210, fn)(p, tags, b);
}
static inline ULONG op_attr(struct Library *b, struct pci_dev *p, ULONG tag)
{
    ULONG value           = 0;
    struct TagItem tags[] = {{tag, (ULONG)&value}, {TAG_DONE, 0}};
    op_attrs(b, p, tags);
    return value;
}
static inline ULONG op_read(struct Library *b, struct pci_dev *p, ULONG reg, unsigned width)
{
    typedef ULONG (*fn)(OREG(ULONG, reg, d0), OREG(struct pci_dev *, p, a0), OREG(struct Library *, b, a6));
    ULONG value = OCALL(b, width == 1 ? 108 : width == 2 ? 114 : 120, fn)(reg, p, b);
    return width == 1 ? value & 255 : width == 2 ? value & 65535 : value;
}
static inline void op_write(struct Library *b, struct pci_dev *p, ULONG reg, ULONG value, unsigned width)
{
    typedef void (*fn)(OREG(ULONG, reg, d0), OREG(ULONG, value, d1), OREG(struct pci_dev *, p, a0),
                       OREG(struct Library *, b, a6));
    OCALL(b, width == 1 ? 126 : width == 2 ? 132 : 138, fn)(reg, value, p, b);
}
static inline BOOL op_obtain(struct Library *b, struct pci_dev *p)
{
    typedef ULONG (*fn)(OREG(struct pci_dev *, p, a0), OREG(struct Library *, b, a6));
    return OCALL(b, 186, fn)(p, b) != 0;
}
static inline void op_release(struct Library *b, struct pci_dev *p)
{
    typedef void (*fn)(OREG(struct pci_dev *, p, a0), OREG(struct Library *, b, a6));
    OCALL(b, 192, fn)(p, b);
}
static inline APTR op_region(struct Library *b, struct pci_dev *p, ULONG address, ULONG size)
{
    typedef APTR (*fn)(OREG(struct pci_dev *, p, a0), OREG(ULONG, address, a1), OREG(ULONG, size, d0),
                       OREG(struct Library *, b, a6));
    return OCALL(b, 246, fn)(p, address, size, b);
}
static inline void op_unregion(struct Library *b, struct pci_dev *p, APTR addr)
{
    typedef void (*fn)(OREG(struct pci_dev *, p, a0), OREG(APTR, addr, a1), OREG(struct Library *, b, a6));
    OCALL(b, 252, fn)(p, addr, b);
}
#endif
