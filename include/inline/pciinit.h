#ifndef AMIGA_INLINE_PCIINIT_H
#define AMIGA_INLINE_PCIINIT_H
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <libraries/pciinit.h>
#define PREG(type, name, reg) type name __asm(#reg)
#define PVEC(offset, type)    ((type)((unsigned char *)PCIInitBase - (offset)))
static inline PCIInitImage *LoadOptionROMFromFile(const char *path, const struct TagItem *tags)
{
    typedef PCIInitImage *(*fn)(PREG(const char *, p, a0), PREG(const struct TagItem *, t, a1),
                                PREG(struct Library *, b, a6));
    return PVEC(30, fn)(path, tags, PCIInitBase);
}
static inline PCIInitImage *LoadOptionROMFromCard(struct pci_dev *board, const struct TagItem *tags)
{
    typedef PCIInitImage *(*fn)(PREG(struct pci_dev *, p, a0), PREG(const struct TagItem *, t, a1),
                                PREG(struct Library *, b, a6));
    return PVEC(36, fn)(board, tags, PCIInitBase);
}
static inline uint32_t FreeOptionROM(PCIInitImage *image)
{
    typedef uint32_t (*fn)(PREG(PCIInitImage *, i, a0), PREG(struct Library *, b, a6));
    return PVEC(42, fn)(image, PCIInitBase);
}
static inline uint32_t GetOptionROMInfo(const PCIInitImage *image, struct PCIInitROMInfo *info)
{
    typedef uint32_t (*fn)(PREG(const PCIInitImage *, i, a0), PREG(struct PCIInitROMInfo *, r, a1),
                           PREG(struct Library *, b, a6));
    return PVEC(48, fn)(image, info, PCIInitBase);
}
static inline PCIInitContext *CreatePCIContext(struct pci_dev *board, const struct TagItem *tags)
{
    typedef PCIInitContext *(*fn)(PREG(struct pci_dev *, p, a0), PREG(const struct TagItem *, t, a1),
                                  PREG(struct Library *, b, a6));
    return PVEC(54, fn)(board, tags, PCIInitBase);
}
static inline uint32_t DeletePCIContext(PCIInitContext *ctx)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(struct Library *, b, a6));
    return PVEC(60, fn)(ctx, PCIInitBase);
}
static inline uint32_t RunOptionROM(PCIInitContext *ctx, const PCIInitImage *image)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(const PCIInitImage *, i, a1),
                           PREG(struct Library *, b, a6));
    return PVEC(66, fn)(ctx, image, PCIInitBase);
}
static inline uint32_t GetPCIContextInfo(const PCIInitContext *ctx, struct PCIInitContextInfo *info)
{
    typedef uint32_t (*fn)(PREG(const PCIInitContext *, c, a0), PREG(struct PCIInitContextInfo *, i, a1),
                           PREG(struct Library *, b, a6));
    return PVEC(72, fn)(ctx, info, PCIInitBase);
}
static inline uint32_t GetVBEInfo(PCIInitContext *ctx, struct PCIInitVBEInfo *info)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(struct PCIInitVBEInfo *, i, a1),
                           PREG(struct Library *, b, a6));
    return PVEC(78, fn)(ctx, info, PCIInitBase);
}
static inline uint32_t GetCurrentVBEMode(PCIInitContext *ctx, uint32_t *mode)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(uint32_t *, i, a1), PREG(struct Library *, b, a6));
    return PVEC(96, fn)(ctx, mode, PCIInitBase);
}
static inline uint32_t MapFramebuffer(PCIInitContext *ctx, struct PCIInitFramebuffer *info)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(struct PCIInitFramebuffer *, i, a1),
                           PREG(struct Library *, b, a6));
    return PVEC(102, fn)(ctx, info, PCIInitBase);
}
static inline uint32_t GetVBEModeInfo(PCIInitContext *ctx, uint32_t mode, struct PCIInitVBEModeInfo *info)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(uint32_t, m, d0),
                           PREG(struct PCIInitVBEModeInfo *, i, a1), PREG(struct Library *, b, a6));
    return PVEC(84, fn)(ctx, mode, info, PCIInitBase);
}
static inline uint32_t SetVBEMode(PCIInitContext *ctx, uint32_t mode, uint32_t flags)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(uint32_t, m, d0), PREG(uint32_t, f, d1),
                           PREG(struct Library *, b, a6));
    return PVEC(90, fn)(ctx, mode, flags, PCIInitBase);
}
static inline uint32_t UnmapFramebuffer(PCIInitContext *ctx)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(struct Library *, b, a6));
    return PVEC(108, fn)(ctx, PCIInitBase);
}
/* Banked-mode pixel transfer through the ROM's VGA window. */
static inline uint32_t WriteFramebuffer(PCIInitContext *ctx, uint32_t offset, const void *source, uint32_t bytes)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(uint32_t, o, d0), PREG(const void *, s, a1),
                           PREG(uint32_t, n, d1), PREG(struct Library *, b, a6));
    return PVEC(114, fn)(ctx, offset, source, bytes, PCIInitBase);
}
static inline uint32_t ReadFramebuffer(PCIInitContext *ctx, uint32_t offset, void *destination, uint32_t bytes)
{
    typedef uint32_t (*fn)(PREG(PCIInitContext *, c, a0), PREG(uint32_t, o, d0), PREG(void *, d, a1),
                           PREG(uint32_t, n, d1), PREG(struct Library *, b, a6));
    return PVEC(120, fn)(ctx, offset, destination, bytes, PCIInitBase);
}
#undef PREG
#undef PVEC
#endif
