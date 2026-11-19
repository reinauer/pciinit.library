#include "private/pciinit.h"
#include "private/vbe.h"
#include <string.h>
static uint32_t ready(struct PIBase *base, PCIInitContext *ctx)
{
    if (!ctx || ctx->owner != base)
        return PI_BAD_ARGUMENT;
    if (ctx->info.state != PI_INITIALIZED || base->mapping_owner)
        return PI_BAD_STATE;
    return PI_OK;
}
uint32_t pi_int10(struct PIBase *base, PCIInitContext *ctx, struct X86Registers *r)
{
    uint32_t status = ready(base, ctx), stop;
    if (status)
        return status;
    if (!(ctx->ram[0x40] | ctx->ram[0x41] | ctx->ram[0x42] | ctx->ram[0x43]))
        return PI_VBE_FAILED;
    if (!ctx->info.offline) {
        status = pi_hardware_start(ctx);
        if (status)
            return status;
    }
    ctx->info.state = PI_EXECUTING;
    if (ctx->diagnostics) {
        ctx->diagnostics->count       = 0;
        ctx->diagnostics->io_accesses = ctx->diagnostics->vga_accesses = ctx->diagnostics->bar_accesses = 0;
    }
    if (ctx->services)
        ctx->services->count = 0;
    ctx->ram[PI_VBE_ENTRY]     = 0xcd;
    ctx->ram[PI_VBE_ENTRY + 1] = 0x10;
    ctx->ram[PI_VBE_ENTRY + 2] = 0xf4;
    r->size                    = sizeof(*r);
    r->eip                     = PI_VBE_ENTRY;
    r->esp                     = 0x6ff0;
    r->eflags                  = 2;
    r->segment[X86_CS] = r->segment[X86_SS] = 0;
    if (x86_set(base->engine, ctx->cpu, r))
        stop = X86_STOP_ACCESS;
    else
        stop = x86_run(base->engine, ctx->cpu, &ctx->limits, &ctx->info.execution);
    x86_get(base->engine, ctx->cpu, r);
    if (ctx->diagnostics) {
        ctx->diagnostics->registers    = *r;
        ctx->diagnostics->int10_vector = ctx->ram[0x40] | ((uint32_t)ctx->ram[0x41] << 8) |
                                         ((uint32_t)ctx->ram[0x42] << 16) | ((uint32_t)ctx->ram[0x43] << 24);
    }
    if (!ctx->info.offline)
        pi_hardware_finish(ctx);
    if (stop != X86_STOP_HALT || r->segment[X86_CS] != 0 || r->eip != PI_VBE_ENTRY + 3) {
        ctx->info.state         = PI_FAILED;
        ctx->mode_valid         = 0;
        return ctx->info.status = PI_EXECUTION_FAILED;
    }
    ctx->info.state         = PI_INITIALIZED;
    return ctx->info.status = (r->eax & 0xffff) == 0x004f ? PI_OK : PI_VBE_FAILED;
}
uint32_t pi_vbe_info(struct PIBase *base, PCIInitContext *ctx, struct PCIInitVBEInfo *out)
{
    struct X86Registers r = {.eax = 0x4f00, .edi = PI_VBE_BUFFER};
    uint32_t status       = ready(base, ctx);
    if (!out || out->size != sizeof(*out))
        return PI_BAD_ARGUMENT;
    if (status)
        return status;
    memset(ctx->ram + PI_VBE_BUFFER, 0, 512);
    memcpy(ctx->ram + PI_VBE_BUFFER, "VBE2", 4);
    status = pi_int10(base, ctx, &r);
    if (!status)
        status = pi_vbe_parse_info(ctx->ram, out);
    if (!status) {
        ctx->vbe_version = out->version;
        ctx->vbe_memory  = out->memory_bytes;
    }
    return status;
}
uint32_t pi_vbe_mode_info(struct PIBase *base, PCIInitContext *ctx, uint32_t mode, struct PCIInitVBEModeInfo *out)
{
    struct X86Registers r = {.eax = 0x4f01, .ecx = mode, .edi = PI_VBE_BUFFER};
    uint32_t status       = ready(base, ctx);
    if (!out || out->size != sizeof(*out) || mode > 0x3fff)
        return PI_BAD_ARGUMENT;
    if (status)
        return status;
    if (!ctx->vbe_version) {
        struct PCIInitVBEInfo info = {.size = sizeof(info)};
        status                     = pi_vbe_info(base, ctx, &info);
        if (status)
            return status;
    }
    memset(ctx->ram + PI_VBE_BUFFER, 0, 256);
    status = pi_int10(base, ctx, &r);
    if (!status)
        status = pi_vbe_parse_mode(ctx->ram, ctx->vbe_version, mode, out);
    return status;
}
uint32_t pi_vbe_current(struct PIBase *base, PCIInitContext *ctx, uint32_t *mode)
{
    struct X86Registers r = {.eax = 0x4f03};
    uint32_t status;
    if (!mode)
        return PI_BAD_ARGUMENT;
    status = pi_int10(base, ctx, &r);
    if (!status)
        *mode = r.ebx & 0xffff;
    return status;
}
uint32_t pi_vbe_set(struct PIBase *base, PCIInitContext *ctx, uint32_t mode, uint32_t flags)
{
    struct PCIInitVBEInfo info  = {.size = sizeof(info)};
    struct PCIInitVBEModeInfo m = {.size = sizeof(m)};
    struct X86Registers r       = {.eax = 0x4f02, .ebx = mode | flags};
    uint32_t status             = ready(base, ctx), current, i;
    if (status)
        return status;
    if (mode > 0x3fff || (flags & ~(PI_VBE_LINEAR | PI_VBE_KEEP_MEMORY)))
        return PI_BAD_ARGUMENT;
    status = pi_vbe_info(base, ctx, &info);
    if (status)
        return status;
    if ((flags & PI_VBE_LINEAR) && info.version < 0x200)
        return PI_UNSUPPORTED_MODE;
    for (i = 0; i < info.mode_count && info.modes[i] != mode; ++i) {
    }
    if (i == info.mode_count)
        return PI_UNSUPPORTED_MODE;
    status = pi_vbe_mode_info(base, ctx, mode, &m);
    if (status)
        return status;
    { /* Window geometry for banked transfers: bytes 2..11 of the mode block. */
        const uint8_t *p          = ctx->ram + PI_VBE_BUFFER;
        ctx->window_attributes[0] = p[2];
        ctx->window_attributes[1] = p[3];
        ctx->window_granularity   = p[4] | ((uint32_t)p[5] << 8);
        ctx->window_size          = p[6] | ((uint32_t)p[7] << 8);
        ctx->window_segment[0]    = p[8] | ((uint32_t)p[9] << 8);
        ctx->window_segment[1]    = p[10] | ((uint32_t)p[11] << 8);
    }
    /* VBE 3 has separate banked and linear pitch/color descriptions. */
    if (!(flags & PI_VBE_LINEAR) && info.version >= 0x300)
        pi_vbe_parse_mode(ctx->ram, 0x200, mode, &m);
    if ((flags & PI_VBE_LINEAR) && (!(m.attributes & 0x80) || !m.framebuffer_pci))
        return PI_UNSUPPORTED_MODE;
    status = pi_vbe_layout(&m, info.memory_bytes);
    if (status)
        return status;
    ctx->mode_valid = 0;
    status          = pi_int10(base, ctx, &r);
    if (status)
        return status;
    status = pi_vbe_current(base, ctx, &current);
    if (status)
        return status;
    if ((current & 0x7fff) != (mode | (flags & PI_VBE_LINEAR)))
        return PI_VBE_FAILED;
    ctx->mode       = m;
    ctx->mode_flags = flags;
    ctx->mode_valid = 1;
    ctx->bank       = 0xffffffffU;
    return PI_OK;
}
/* Banked modes have no flat window: copy between caller memory and the
 * visible pixels through the ROM's VGA window, selecting banks with VBE
 * function 05h. The window must lie inside the bridge's legacy region. */
uint32_t pi_fb_transfer(struct PIBase *base, PCIInitContext *ctx, uint32_t offset, void *buffer, uint32_t bytes,
                        unsigned write)
{
    uint32_t status = ready(base, ctx), current, total, granularity, size, window_base;
    unsigned window, need = write ? 4 : 2;
    uint8_t *data = buffer;
    if (status)
        return status;
    if (!buffer || !bytes)
        return PI_BAD_ARGUMENT;
    if (!ctx->mode_valid || !ctx->board || !ctx->hardware)
        return PI_BAD_STATE;
    status = pi_vbe_layout(&ctx->mode, ctx->vbe_memory);
    if (status)
        return status;
    total = ctx->mode.pitch * ctx->mode.height;
    if (offset > total || bytes > total - offset)
        return PI_BAD_ARGUMENT;
    /* MapFramebuffer serves linear modes. */
    if (ctx->mode_flags & PI_VBE_LINEAR)
        return PI_UNSUPPORTED_MAPPING;
    status = pi_vbe_current(base, ctx, &current);
    if (status)
        return status;
    if ((current & 0x7fff) != ctx->mode.mode) {
        ctx->mode_valid = 0;
        return PI_BAD_STATE;
    }
    window      = (ctx->window_attributes[0] & need) ? 0 : (ctx->window_attributes[1] & need) ? 1 : 2;
    granularity = ctx->window_granularity * 1024;
    size        = ctx->window_size * 1024;
    window_base = (uint32_t)ctx->window_segment[window & 1] << 4;
    if (window == 2 || !granularity || !size || granularity > size || window_base < PI_LEGACY_BASE ||
        window_base - PI_LEGACY_BASE > PI_LEGACY_BYTES - size)
        return PI_UNSUPPORTED_MAPPING;
    while (bytes) {
        uint32_t bank = offset / granularity, within = offset - bank * granularity, chunk = size - within, i = 0,
                 faults;
        volatile uint8_t *cpu;
        if (chunk > bytes)
            chunk = bytes;
        if (bank != ctx->bank) {
            struct X86Registers r = {.eax = 0x4f05, .ebx = window, .edx = bank};
            status                = pi_int10(base, ctx, &r);
            if (status)
                return status;
            ctx->bank = bank;
        }
        status = pi_hardware_start(ctx);
        if (status)
            return status;
        if (!ctx->legacy) {
            pi_hardware_finish(ctx);
            return PI_UNSUPPORTED_MAPPING;
        }
        cpu = (volatile uint8_t *)ctx->legacy + (window_base - PI_LEGACY_BASE) + within;
        /* Byte order is preserved either way; longwords only cut bus cycles.
         * The bus routines end the chunk at a bus error (see bus.S). */
        faults = ctx->trap.faults;
        if (!(((uint32_t)cpu | (uint32_t)data) & 3) && chunk >= 4) {
            i = chunk & ~3U;
            if (write)
                pi_bus_copy32(cpu, data, i / 4);
            else
                pi_bus_copy32(data, (const void *)cpu, i / 4);
        }
        if (i < chunk && ctx->trap.faults == faults) {
            if (write)
                pi_bus_copy8(cpu + i, data + i, chunk - i);
            else
                pi_bus_copy8(data + i, (const void *)(cpu + i), chunk - i);
        }
        faults = ctx->trap.faults != faults;
        pi_hardware_finish(ctx);
        if (faults)
            return PI_IO_ERROR;
        offset += chunk;
        data += chunk;
        bytes -= chunk;
    }
    return PI_OK;
}
uint32_t pi_fb_map(struct PIBase *base, PCIInitContext *ctx, struct PCIInitFramebuffer *out)
{
    struct PCIInitFramebuffer fb = {.size = sizeof(fb)};
    uint32_t status              = ready(base, ctx), current, command;
    unsigned i;
    if (!out || out->size != sizeof(*out))
        return PI_BAD_ARGUMENT;
    if (status)
        return status;
    if (!ctx->mode_valid || !ctx->board || !ctx->hardware)
        return PI_BAD_STATE;
    status = pi_vbe_current(base, ctx, &current);
    if (status)
        return status;
    if ((current & 0x7fff) != (ctx->mode.mode | (ctx->mode_flags & PI_VBE_LINEAR))) {
        ctx->mode_valid = 0;
        return PI_BAD_STATE;
    }
    status = pi_vbe_layout(&ctx->mode, ctx->vbe_memory);
    if (status)
        return status;
    fb.mode  = ctx->mode;
    fb.flags = PI_FB_LITTLE_ENDIAN | (ctx->mode.memory_model == 4 ? PI_FB_INDEXED : 0);
    fb.bytes = ctx->mode.pitch * ctx->mode.height;
    /* Only a ROM-advertised linear framebuffer inside one of the card's
     * assigned memory BARs is mapped. A banked mode has no flat window. */
    if (!(ctx->mode_flags & PI_VBE_LINEAR) || !fb.mode.framebuffer_pci)
        return PI_UNSUPPORTED_MAPPING;
    for (i = 0; i < ctx->bar_count; ++i) {
        const struct PIBar *b = &ctx->bars[i];
        if (fb.mode.framebuffer_pci >= b->bus && fb.mode.framebuffer_pci - b->bus < b->size &&
            fb.bytes <= b->size - (fb.mode.framebuffer_pci - b->bus))
            break;
    }
    if (i == ctx->bar_count)
        return PI_UNSUPPORTED_MAPPING;
    command = op_read(ctx->pci, ctx->board, PCI_COMMAND, 2);
    op_write(ctx->pci, ctx->board, PCI_COMMAND, (command | 3) & ~PCI_COMMAND_MASTER, 2);
    if (op_read(ctx->pci, ctx->board, PCI_COMMAND, 2) != ((command | 3) & ~PCI_COMMAND_MASTER)) {
        op_write(ctx->pci, ctx->board, PCI_COMMAND, command, 2);
        return PI_IO_ERROR;
    }
    fb.address = op_region(ctx->pci, ctx->board, fb.mode.framebuffer_pci, fb.bytes);
    if (!fb.address) {
        op_write(ctx->pci, ctx->board, PCI_COMMAND, command, 2);
        return PI_UNSUPPORTED_MAPPING;
    }
    ctx->map_command    = command;
    ctx->framebuffer    = (APTR)fb.address;
    base->mapping_owner = ctx;
    *out                = fb;
    return PI_OK;
}
uint32_t pi_fb_unmap(struct PIBase *base, PCIInitContext *ctx)
{
    if (!ctx || ctx->owner != base)
        return PI_BAD_ARGUMENT;
    if (!ctx->framebuffer || base->mapping_owner != ctx || ctx->info.state == PI_EXECUTING)
        return PI_BAD_STATE;
    op_unregion(ctx->pci, ctx->board, ctx->framebuffer);
    ctx->framebuffer    = NULL;
    base->mapping_owner = NULL;
    op_write(ctx->pci, ctx->board, PCI_COMMAND, ctx->map_command, 2);
    return op_read(ctx->pci, ctx->board, PCI_COMMAND, 2) == ctx->map_command ? PI_OK : PI_IO_ERROR;
}
