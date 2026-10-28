#include "private/pciinit.h"
#include "private/quirks.h"
#include "private/record.h"
#include <proto/exec.h>
#include <string.h>

/* The fixed entry points of the IBM system BIOS, which option ROMs install
 * into the IVT or call directly: Cirrus VGA ROMs point INT 42h at
 * F000:F065 and call it to set the text mode. Each holds an IRET, so such
 * a call returns instead of executing zero bytes. FFFE is the AT model
 * byte. The segment is otherwise empty and read-only. */
static void bios_stubs(uint8_t *segment)
{
    static const uint16_t entries[] = {0xe2c3, 0xe3fe, 0xe6f2, 0xe739, 0xe82e, 0xe987, 0xefd2, 0xf065,
                                       0xf841, 0xf84d, 0xf859, 0xfe6e, 0xfea5, 0xff53, 0xff54, 0xfff0};
    unsigned i;
    for (i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
        segment[entries[i]] = 0xcf;
    segment[0xfffe] = 0xfc;
}
static uint32_t X86_CALLBACK memory(void *user, uint32_t address, uint32_t width, uint32_t operation, uint32_t *value)
{
    PCIInitContext *ctx = user;
    uint32_t i;
    if (width != 1 && width != 2 && width != 4)
        return 1;
    if (address > 0xffffffffU - width)
        return 1;
    if (operation > X86_FETCH || address >= PI_RAM_BYTES || width > PI_RAM_BYTES - address ||
        (address < 0xc0000 && address + width > 0xa0000)) {
        if (ctx->hardware && ctx->board)
            return pi_hardware_access(ctx, address, width, operation, value);
        return 1;
    }
    if (operation == X86_WRITE) {
        if (address + width > 0xf0000)
            return 1;
        for (i = 0; i < width; ++i)
            ctx->ram[address + i] = *value >> (8 * i);
    } else {
        *value = 0;
        for (i = 0; i < width; ++i)
            *value |= (uint32_t)ctx->ram[address + i] << (8 * i);
    }
    return 0;
}
/* RAM and VRAM remain distinct. This callback batches contiguous forward
 * ranges and completes the ordinary-RAM prefix of a request; the VGA
 * aperture, the end of RAM and (for writes) the ROM image end a span.
 * Reverse/overlap/device semantics retain scalar execution. */
static uint32_t ram_prefix(uint32_t address, uint32_t bytes, int write)
{
    uint32_t end = write ? 0xf0000 : PI_RAM_BYTES;
    if (address < 0xa0000)
        end = 0xa0000;
    else if (address < 0xc0000 || address >= end)
        return 0;
    return bytes < end - address ? bytes : end - address;
}
static uint32_t X86_CALLBACK bulk(void *user, const struct X86BulkRequest *b)
{
    PCIInitContext *c       = user;
    struct X86BulkRequest r = *b;
    uint32_t bytes          = r.width * r.count, prefix, i;
    const uint8_t *source   = NULL;
    if (r.stride != (int32_t)r.width || !r.count)
        return 0;
    if (r.operation == X86_BULK_COPY) {
        prefix = ram_prefix(r.source, bytes, 0);
        if (prefix < r.width)
            return 0;
        if (prefix < bytes) {
            r.count = prefix / r.width;
            bytes   = r.count * r.width;
        }
        source = c->ram + r.source;
    }
    prefix = ram_prefix(r.destination, bytes, 1);
    if (prefix) {
        uint8_t *dest = c->ram + r.destination;
        if (prefix < r.width)
            return 0;
        if (prefix < bytes) {
            r.count = prefix / r.width;
            bytes   = r.count * r.width;
        }
        if (source) {
            if (r.source < r.destination + bytes && r.destination < r.source + bytes)
                return 0;
            memcpy(dest, source, bytes);
        } else if (r.width == 1 || !r.value)
            memset(dest, r.value & 255, bytes);
        else
            for (i = 0; i < bytes; ++i)
                dest[i] = r.value >> (8 * (i % r.width));
        return r.count;
    }
    if (c->hardware && c->board)
        return pi_hardware_bulk(c, &r, source);
    return 0;
}
static uint32_t X86_CALLBACK scan(void *user, const struct X86ScanRequest *s, uint32_t *last)
{
    PCIInitContext *c = user;
    if (c->hardware && c->board)
        return pi_hardware_scan(c, s, last);
    return 0;
}
#ifdef PCIINIT_RECORD
#define interrupt interrupt_recorded_body
#endif
static uint32_t X86_CALLBACK interrupt(void *user, uint32_t number, uint32_t fault, struct X86Registers *r)
{
    PCIInitContext *ctx = user;
    uint8_t *vector;
    uint32_t handled;
    pi_trace(ctx, PI_TRACE_INTERRUPT, number, fault, r->eax);
    if (fault || number > 255)
        return 2;
    vector = ctx->ram + number * 4;
    if (vector[0] || vector[1] || vector[2] || vector[3])
        return 0;
    /* Minimal synthetic environment; a service nobody provides stops the
     * run and is named for the caller's report. */
    if (number == 0x12) {
        r->eax  = (r->eax & 0xffff0000) | 640;
        handled = 1;
    } else if (number == 0x11) {
        r->eax  = (r->eax & 0xffff0000) | 0x20;
        handled = 1;
    } else if (ctx->hardware && ctx->board)
        handled = pi_bios(ctx, number, r);
    else
        handled = 2;
    if (handled == 2 && !(ctx->info.warnings & PI_WARN_MISSING_SERVICE)) {
        ctx->info.warnings |= PI_WARN_MISSING_SERVICE;
        ctx->info.missing_service = (number << 16) | (r->eax & 0xffff);
    }
    return handled;
}
#ifdef PCIINIT_RECORD
#undef interrupt
static uint32_t X86_CALLBACK interrupt(void *user, uint32_t number, uint32_t fault, struct X86Registers *r)
{
    struct PIRecordInterrupt record = {.number = number, .fault = fault, .before = *r};
    record.handled                  = interrupt_recorded_body(user, number, fault, r);
    record.after                    = *r;
    pi_record(PI_RECORD_INTERRUPT, &record, sizeof(record));
    return record.handled;
}
#endif
static int valid_limits(const struct X86Limits *l)
{
    return l && l->size == sizeof(*l) && l->instructions && l->accesses && l->milliseconds;
}
PCIInitContext *pi_create(struct PIBase *base, struct pci_dev *board, const struct TagItem *tags)
{
    struct ExecBase *SysBase = base->sys;
    struct PIOptions opts;
    PCIInitContext *ctx = NULL;
    uint32_t status     = pi_options(tags, &opts);
    if (status)
        goto fail;
    if ((opts.offline && (board || opts.hardware)) || (!opts.offline && !board) ||
        (opts.diagnostics &&
         (opts.diagnostics->size != sizeof(*opts.diagnostics) || opts.diagnostics->capacity > 4096 ||
          (opts.diagnostics->capacity && !opts.diagnostics->entries))) ||
        (opts.limits && !valid_limits(opts.limits))) {
        status = PI_BAD_ARGUMENT;
        goto fail;
    }
    if (board && base->mapping_owner) {
        status = PI_BAD_STATE;
        goto fail;
    }
    ctx = pi_alloc(sizeof(*ctx));
    if (!ctx) {
        status = PI_NO_MEMORY;
        goto fail;
    }
    ctx->owner               = base;
    ctx->hardware            = opts.hardware;
    ctx->diagnostics         = opts.diagnostics;
    ctx->quirks              = !!opts.quirks;
    ctx->info.size           = sizeof(ctx->info);
    ctx->info.offline        = !!opts.offline;
    ctx->info.execution.size = sizeof(ctx->info.execution);
    ctx->limits              = (struct X86Limits){sizeof(ctx->limits), 1000000, 10000000, 5000, NULL};
    if (opts.hardware)
        ctx->limits = (struct X86Limits){sizeof(ctx->limits), 10000000, 30000000, 300000, NULL};
    if (opts.limits)
        ctx->limits = *opts.limits;
    if (board) {
        ctx->pci = op_open(SysBase);
        if (!ctx->pci) {
            status = PI_NO_OPENPCI;
            goto fail;
        }
        if (!pi_board_exists(ctx->pci, board)) {
            status = PI_BAD_ARGUMENT;
            goto fail;
        }
        if (!op_obtain(ctx->pci, board)) {
            status = PI_CARD_BUSY;
            goto fail;
        }
        ctx->board  = board;
        ctx->vendor = op_read(ctx->pci, board, 0, 2);
        ctx->device = op_read(ctx->pci, board, 2, 2);
    }
    ctx->ram = pi_alloc(PI_RAM_BYTES);
    if (!ctx->ram) {
        status = PI_NO_MEMORY;
        goto fail;
    }
    {
        /* Only owned ordinary RAM: the VGA aperture stays callback-only. Data
         * accesses to the first two regions bypass memory(); the last keeps
         * its write rejection in the callback. Requests span whole real-mode
         * segments, and the callbacks complete prefixes. */
        const struct X86CodeRegion regions[] = {{0, 0xa0000, ctx->ram},
                                                {0xc0000, 0xf0000 - 0xc0000, ctx->ram + 0xc0000},
                                                {0xf0000, PI_RAM_BYTES - 0xf0000, ctx->ram + 0xf0000}};
        const struct X86Config config = {sizeof(config), X86EMU_ABI_VERSION, memory, interrupt, ctx, 3, regions, bulk,
                                         scan,           X86_SPAN_MAX,       3};
        ctx->cpu                      = x86_create(base->engine, &config);
    }
    if (!ctx->cpu) {
        status = PI_NO_MEMORY;
        goto fail;
    }
    ++base->objects;
    if (opts.error)
        *opts.error = PI_OK;
    return ctx;
fail:
    if (ctx) {
        pi_free(ctx->ram);
        if (ctx->board)
            op_release(ctx->pci, ctx->board);
        if (ctx->pci)
            CloseLibrary(ctx->pci);
        pi_free(ctx);
    }
    if (opts.error)
        *opts.error = status;
    return NULL;
}
uint32_t pi_delete(struct PIBase *base, PCIInitContext *ctx)
{
    struct ExecBase *SysBase = base->sys;
    if (!ctx || ctx->owner != base)
        return PI_BAD_ARGUMENT;
    if (ctx->info.state == PI_EXECUTING || ctx->framebuffer)
        return PI_BAD_STATE;
    if (x86_delete(base->engine, ctx->cpu))
        return PI_BAD_STATE;
    pi_free(ctx->ram);
    if (ctx->board)
        op_release(ctx->pci, ctx->board);
    if (ctx->pci)
        CloseLibrary(ctx->pci);
    pi_free(ctx);
    --base->objects;
    return PI_OK;
}
uint32_t pi_run(struct PIBase *base, PCIInitContext *ctx, const PCIInitImage *image)
{
    struct X86Registers r = {.size = sizeof(r), .eip = 3, .esp = 0x6ff0, .eflags = 2};
    uint32_t stop;
    if (!ctx || ctx->owner != base || !image || image->owner != base)
        return PI_BAD_ARGUMENT;
    if (ctx->info.state != PI_CREATED)
        return PI_BAD_STATE;
    if (ctx->board && (ctx->vendor != image->info.vendor || ctx->device != image->info.device))
        return ctx->info.status = PI_WRONG_CARD;
    if (image->info.init_bytes > 0x30000)
        return ctx->info.status = PI_UNSUPPORTED_ROM;
#ifdef PCIINIT_RECORD
    pi_record_mark(0);
#endif
    ctx->info.warnings        = 0;
    ctx->info.missing_service = 0;
    if (!ctx->info.offline) {
        uint32_t status = pi_hardware_start(ctx);
        if (status)
            return ctx->info.status = status;
        r.eax = ctx->bdf;
    }
#ifdef PCIINIT_RECORD
    pi_record_mark(1);
#endif
    if (ctx->diagnostics) {
        ctx->diagnostics->count       = 0;
        ctx->diagnostics->io_accesses = ctx->diagnostics->vga_accesses = ctx->diagnostics->bar_accesses = 0;
    }
    ctx->info.state = PI_EXECUTING;
    memset(ctx->ram, 0, PI_RAM_BYTES);
    memcpy(ctx->ram + PI_ROM_BASE, image->data + image->info.image_offset, image->info.init_bytes);
    if (ctx->quirks) {
        /* Known images are adjusted in RAM only; the card ROM is unchanged. */
        const struct PIQuirk *quirk = pi_find_quirk(image->crc, image->info.init_bytes);
        if (quirk)
            pi_apply_patches(ctx->ram + PI_ROM_BASE, image->info.init_bytes, quirk->patches);
    }
    ctx->ram[0x410]     = 0x20; /* same equipment flags as INT 11h */
    ctx->ram[0x413]     = 0x80;
    ctx->ram[0x414]     = 2; /* 640 KiB */
    ctx->ram[PI_RETURN] = 0xf4;
    ctx->ram[0x6ff0]    = PI_RETURN & 255;
    ctx->ram[0x6ff1]    = PI_RETURN >> 8;
    bios_stubs(ctx->ram + 0xf0000);
    r.segment[X86_CS] = 0xc000;
#ifdef PCIINIT_RECORD
    pi_record(PI_RECORD_RAM, ctx->ram, PI_RAM_BYTES);
    pi_record(PI_RECORD_REGS, &r, sizeof(r));
#endif
    x86_set(base->engine, ctx->cpu, &r);
#ifdef PCIINIT_RECORD
    pi_record_mark(2);
#endif
    stop = x86_run(base->engine, ctx->cpu, &ctx->limits, &ctx->info.execution);
    x86_get(base->engine, ctx->cpu, &r);
#ifdef PCIINIT_RECORD
    pi_record_mark(3);
    pi_record(PI_RECORD_FINAL_RAM, ctx->ram, PI_RAM_BYTES);
    pi_record(PI_RECORD_FINAL_REGS, &r, sizeof(r));
    pi_record(PI_RECORD_RESULT, &ctx->info.execution, sizeof(ctx->info.execution));
#endif
    if (ctx->diagnostics) {
        ctx->diagnostics->registers    = r;
        ctx->diagnostics->int10_vector = ctx->ram[0x40] | ((uint32_t)ctx->ram[0x41] << 8) |
                                         ((uint32_t)ctx->ram[0x42] << 16) | ((uint32_t)ctx->ram[0x43] << 24);
    }
#ifdef PCIINIT_RECORD
    pi_record_mark(4);
#endif
    if (ctx->hardware && ctx->board)
        pi_hardware_finish(ctx);
#ifdef PCIINIT_RECORD
    pi_record_mark(5);
#endif
    if (stop == X86_STOP_HALT && r.segment[X86_CS] == 0 && r.eip == PI_RETURN + 1) {
        ctx->info.state  = PI_INITIALIZED;
        ctx->info.status = PI_OK;
    } else {
        ctx->info.state  = PI_FAILED;
        ctx->info.status = PI_EXECUTION_FAILED;
    }
    return ctx->info.status;
}
