#include "private/pciinit.h"
#include "private/version.h"
#include <stddef.h>
#include <string.h>
const char lib_name[]  = "pciinit.library";
const char lib_ident[] = PCIINIT_IDSTRING;
static PCIInitImage *load_file(REG(const char *, path, a0), REG(const struct TagItem *, tags, a1), BASE)
{
    return pi_load_file(base, path, tags);
}
static PCIInitImage *load_card(REG(struct pci_dev *, board, a0), REG(const struct TagItem *, tags, a1), BASE)
{
    return pi_load_card(base, board, tags);
}
static ULONG free_image(REG(PCIInitImage *, image, a0), BASE)
{
    return pi_free_image(base, image);
}
static ULONG image_info(REG(const PCIInitImage *, image, a0), REG(struct PCIInitROMInfo *, info, a1), BASE)
{
    if (!image || image->owner != base || !info || info->size != sizeof(*info))
        return PI_BAD_ARGUMENT;
    *info = image->info;
    return PI_OK;
}
static PCIInitContext *create(REG(struct pci_dev *, board, a0), REG(const struct TagItem *, tags, a1), BASE)
{
    return pi_create(base, board, tags);
}
static ULONG destroy(REG(PCIInitContext *, ctx, a0), BASE)
{
    return pi_delete(base, ctx);
}
static ULONG run(REG(PCIInitContext *, ctx, a0), REG(const PCIInitImage *, image, a1), BASE)
{
    return pi_run(base, ctx, image);
}
/* Callers built before the bus error fields pass the shorter size. */
#define PI_CONTEXT_INFO_V23 offsetof(struct PCIInitContextInfo, bus_errors)
static ULONG context_info(REG(const PCIInitContext *, ctx, a0), REG(struct PCIInitContextInfo *, info, a1), BASE)
{
    uint32_t size;
    if (!ctx || ctx->owner != base || !info)
        return PI_BAD_ARGUMENT;
    size = info->size;
    if (size != sizeof(*info) && size != PI_CONTEXT_INFO_V23)
        return PI_BAD_ARGUMENT;
    if (ctx->info.state == PI_EXECUTING)
        return PI_BAD_STATE;
    memcpy(info, &ctx->info, size);
    info->size = size;
    return PI_OK;
}
static ULONG vbe_info(REG(PCIInitContext *, ctx, a0), REG(struct PCIInitVBEInfo *, info, a1), BASE)
{
    return pi_vbe_info(base, ctx, info);
}
static ULONG mode_info(REG(PCIInitContext *, ctx, a0), REG(ULONG, mode, d0), REG(struct PCIInitVBEModeInfo *, info, a1),
                       BASE)
{
    return pi_vbe_mode_info(base, ctx, mode, info);
}
static ULONG set_mode(REG(PCIInitContext *, ctx, a0), REG(ULONG, mode, d0), REG(ULONG, flags, d1), BASE)
{
    return pi_vbe_set(base, ctx, mode, flags);
}
static ULONG get_mode(REG(PCIInitContext *, ctx, a0), REG(uint32_t *, mode, a1), BASE)
{
    return pi_vbe_current(base, ctx, mode);
}
static ULONG map_fb(REG(PCIInitContext *, ctx, a0), REG(struct PCIInitFramebuffer *, fb, a1), BASE)
{
    return pi_fb_map(base, ctx, fb);
}
static ULONG unmap_fb(REG(PCIInitContext *, ctx, a0), BASE)
{
    return pi_fb_unmap(base, ctx);
}
static ULONG write_fb(REG(PCIInitContext *, ctx, a0), REG(ULONG, offset, d0), REG(const void *, source, a1),
                      REG(ULONG, bytes, d1), BASE)
{
    return pi_fb_transfer(base, ctx, offset, (void *)source, bytes, 1);
}
static ULONG read_fb(REG(PCIInitContext *, ctx, a0), REG(ULONG, offset, d0), REG(void *, destination, a1),
                     REG(ULONG, bytes, d1), BASE)
{
    return pi_fb_transfer(base, ctx, offset, destination, bytes, 0);
}
const APTR lib_vectors[] = {lib_open,   lib_close, lib_expunge, lib_reserved, load_file,    load_card, free_image,
                            image_info, create,    destroy,     run,          context_info, vbe_info,  mode_info,
                            set_mode,   get_mode,  map_fb,      unmap_fb,     write_fb,     read_fb,   (APTR)-1};
