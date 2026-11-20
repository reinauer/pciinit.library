#include <proto/exec.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <utility/hooks.h>
#include <proto/pciinit.h>
#include "private/openpci_calls.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "private/pattern.h"
#include "private/boing.h"
#include "private/oprom_config.h"
#include "private/accesslog.h"
struct Library *PCIInitBase;
static const char *status_name(uint32_t status)
{
    static const char *const names[] = {"ok",
                                        "bad argument",
                                        "out of memory",
                                        "I/O error",
                                        "malformed ROM",
                                        "unsupported ROM",
                                        "OpenPCI unavailable",
                                        "card owned",
                                        "no ROM",
                                        "ROM identity mismatch",
                                        "hardware execution backend unavailable",
                                        "invalid context state",
                                        "execution failed",
                                        "VBE service failed",
                                        "unsupported graphics mode"};
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown status";
}
static int list_modes, pattern, no_post, trace_accesses, log_services, release_vga;
static uint32_t service_entries[32];
static struct PCIInitServiceLog services = {sizeof(services), 32, 0, service_entries};
static uint32_t modes[16];
static unsigned mode_count;
static const char *rom_path, *config_path, *dump_path, *log_path;
/* --log: every card access of each POST and BIOS call, through an access
 * hook that exists only then, so runs without --log are unaffected. */
static struct AccessLog *access_log;
static FILE *log_file;
static int log_failed;
static ULONG log_entry(struct Hook *hook __asm("a0"), APTR object __asm("a2"), struct PCIInitAccessMsg *m __asm("a1"))
{
    (void)hook;
    (void)object;
    /* Writes as they go out, reads with the value that came back. */
    if (m->size == sizeof(*m) && (m->phase == PI_PHASE_AFTER || m->write))
        accesslog_add(access_log, m->space, m->bar, m->offset, m->width, m->write, m->value);
    return PI_HOOK_PASS;
}
static struct Hook log_hook = {{NULL, NULL}, (ULONG (*)())(APTR)log_entry, NULL, NULL};
static void log_mark(const char *label)
{
    if (access_log)
        accesslog_mark(access_log, label);
}
/* Between library calls only: the log is written with DOS. */
static void log_flush(void)
{
    if (access_log && accesslog_write(access_log, log_file))
        log_failed = 1;
}
static int arguments(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-post"))
            no_post = 1;
        else if (!strcmp(argv[i], "--list-modes"))
            list_modes = 1;
        else if (!strcmp(argv[i], "--pattern"))
            pattern = 1;
        else if (!strcmp(argv[i], "--trace"))
            trace_accesses = 1;
        else if (!strcmp(argv[i], "--services"))
            log_services = 1;
        else if (!strcmp(argv[i], "--release-vga"))
            release_vga = 1;
        else if (!strcmp(argv[i], "--config")) {
            if (++i == argc || config_path)
                return 0;
            config_path = argv[i];
        } else if (!strcmp(argv[i], "--log")) {
            if (++i == argc || log_path)
                return 0;
            log_path = argv[i];
        } else if (!strcmp(argv[i], "--dump")) {
            if (++i == argc || dump_path)
                return 0;
            dump_path = argv[i];
        } else if (!strcmp(argv[i], "--mode")) {
            char *end;
            unsigned long mode;
            if (++i == argc || mode_count == 16 || !argv[i][0])
                return 0;
            errno = 0;
            mode  = strtoul(argv[i], &end, 0);
            if (errno || *end || mode > 0x3fff)
                return 0;
            modes[mode_count++] = mode;
        } else if (argv[i][0] == '-' || rom_path)
            return 0;
        else
            rom_path = argv[i];
    }
    return !(no_post && (list_modes || mode_count || pattern)) && !(pattern && !mode_count) && !(dump_path && rom_path);
}
/* A banked mode: draw into RAM, then copy through the ROM's VGA window. */
static uint32_t banked(PCIInitContext *ctx, const struct PCIInitVBEModeInfo *mode, const struct BoingBall *ball)
{
    struct PCIInitFramebuffer fb = {.size = sizeof(fb), .mode = *mode};
    uint8_t *backing, *row = NULL;
    uint32_t status, y;
    int drawn;
    fb.bytes   = mode->pitch * mode->height;
    fb.flags   = PI_FB_LITTLE_ENDIAN | (mode->memory_model == 4 ? PI_FB_INDEXED : 0);
    backing    = malloc(fb.bytes);
    fb.address = backing;
    if (!backing)
        return PI_NO_MEMORY;
    drawn  = pattern ? draw_pattern(&fb) : boing_draw(ball, &fb, 0);
    status = drawn ? WriteFramebuffer(ctx, 0, backing, fb.bytes) : PI_OK;
    if (status == PI_UNSUPPORTED_MAPPING) {
        free(backing);
        puts("  DISPLAY: VBE 1.2 banked mode and this bus board has no legacy VGA window;");
        puts("  nothing drawn (a VBE 2.0 ROM with a linear framebuffer would draw)");
        return PI_OK;
    }
    /* Verify through the window: the first, middle and last rows. */
    if (!status && drawn && pattern && (row = malloc(mode->pitch)))
        for (y = 0; y < 3 && drawn; ++y) {
            uint32_t line = y == 0 ? 0 : y == 1 ? mode->height / 2 : mode->height - 1;
            status        = ReadFramebuffer(ctx, line * mode->pitch, row, mode->pitch);
            if (status)
                break;
            drawn = !memcmp(row, backing + line * mode->pitch, mode->pitch);
        }
    free(row);
    free(backing);
    if (status)
        return status;
    printf("  FRAMEBUFFER banked bytes=%lu flags=%lu %s=%s\n", (ULONG)fb.bytes, (ULONG)fb.flags,
           pattern ? "pattern" : "boing", drawn ? (pattern ? "verified" : "drawn") : "FAILED");
    return drawn ? PI_OK : PI_IO_ERROR;
}
static uint32_t display(PCIInitContext *ctx, const struct PCIInitVBEModeInfo *mode)
{
    struct PCIInitFramebuffer fb = {.size = sizeof(fb)};
    struct BoingBall ball        = {0};
    uint32_t status;
    int drawn;
    if (!pattern && mode->memory_model != 6) {
        puts("  DISPLAY: Boing Ball requires a direct-color mode");
        return PI_OK;
    }
    /* LUT allocation and floating-point initialization precede the mapping. */
    if (!pattern && !boing_prepare(&ball, mode->width, mode->height))
        return PI_NO_MEMORY;
    status = MapFramebuffer(ctx, &fb);
    if (status == PI_UNSUPPORTED_MAPPING) {
        uint32_t result = banked(ctx, mode, &ball);
        boing_release(&ball);
        return result;
    }
    if (status) {
        boing_release(&ball);
        return status;
    }
    drawn  = pattern ? draw_pattern(&fb) : boing_draw(&ball, &fb, 0);
    status = UnmapFramebuffer(ctx);
    boing_release(&ball);
    if (status)
        return status;
    printf("  FRAMEBUFFER PCI=%08lx CPU=%08lx bytes=%lu flags=%lu %s=%s\n", (ULONG)fb.mode.framebuffer_pci,
           (ULONG)fb.address, (ULONG)fb.bytes, (ULONG)fb.flags, pattern ? "pattern" : "boing",
           drawn ? (pattern ? "verified" : "drawn") : "FAILED");
    return drawn ? PI_OK : PI_IO_ERROR;
}
static uint32_t graphics(PCIInitContext *ctx)
{
    struct PCIInitVBEInfo info   = {.size = sizeof(info)};
    struct PCIInitVBEModeInfo mi = {.size = sizeof(mi)};
    uint32_t status, current, flags;
    unsigned i;
    if (!list_modes && !mode_count)
        return PI_OK;
    log_mark("VBE controller and mode information");
    status = GetVBEInfo(ctx, &info);
    if (status)
        return status;
    printf("  VBE %u.%u OEM=%s memory=%lu modes=%lu\n", (unsigned)(info.version >> 8), (unsigned)(info.version & 255),
           info.oem, (ULONG)info.memory_bytes, (ULONG)info.mode_count);
    if (list_modes)
        for (i = 0; i < info.mode_count; ++i) {
            status = GetVBEModeInfo(ctx, info.modes[i], &mi);
            if (status)
                return status;
            printf("  MODE 0x%03lx %lux%lux%lu pitch=%lu model=%lu attributes=%04lx PCI=%08lx\n", (ULONG)mi.mode,
                   (ULONG)mi.width, (ULONG)mi.height, (ULONG)mi.bits_per_pixel, (ULONG)mi.pitch, (ULONG)mi.memory_model,
                   (ULONG)mi.attributes, (ULONG)mi.framebuffer_pci);
        }
    for (i = 0; i < mode_count; ++i) {
        {
            char label[32];
            snprintf(label, sizeof(label), "mode 0x%03lx", (ULONG)modes[i]);
            log_mark(label);
        }
        status = GetVBEModeInfo(ctx, modes[i], &mi);
        if (status)
            return status;
        if (pattern && mi.memory_model != 6)
            return PI_UNSUPPORTED_MODE;
        flags = info.version >= 0x200 && (mi.attributes & 0x80) ? PI_VBE_LINEAR : 0;
        /* Both direct-color displays repaint the complete visible area. */
        if (mi.memory_model == 6)
            flags |= PI_VBE_KEEP_MEMORY;
        status = SetVBEMode(ctx, modes[i], flags);
        if (status)
            return status;
        status = GetCurrentVBEMode(ctx, &current);
        if (status)
            return status;
        printf("  SET 0x%03lx current=%04lx %lux%lux%lu pitch=%lu\n", (ULONG)modes[i], (ULONG)current, (ULONG)mi.width,
               (ULONG)mi.height, (ULONG)mi.bits_per_pixel, (ULONG)mi.pitch);
        status = display(ctx, &mi);
        if (status)
            return status;
    }
    status = GetCurrentVBEMode(ctx, &current);
    if (!status)
        printf("  CURRENT %04lx\n", (ULONG)current);
    return status;
}
/* What the ROM reached since POST started: drivers can tell whether the
 * card needs the bus board's I/O window and legacy VGA after POST. */
static void legacy_line(PCIInitContext *ctx, const char *when)
{
    struct PCIInitContextInfo ci = {.size = sizeof(ci)};
    if (GetPCIContextInfo(ctx, &ci))
        return;
    printf("  LEGACY%s: VGA ports %s, other I/O ports %s, VGA memory %s\n", when,
           ci.legacy_used & PI_USED_VGA_PORTS ? "used" : "unused",
           ci.legacy_used & PI_USED_IO_PORTS ? "used" : "unused",
           ci.legacy_used & PI_USED_VGA_MEMORY ? "used" : "unused");
}
/* Execution result, final registers and, with --trace, the retained access
 * trace of the last ROM run or BIOS call; returns whether the legacy window
 * was missed. Without --trace the diagnostics record no entries. */
static int report(PCIInitContext *ctx, const struct PCIInitDiagnostics *diag)
{
    struct PCIInitContextInfo ci = {.size = sizeof(ci)};
    unsigned n, start, missed = 0;
    /* A 2.3 library takes only the structure without the bus error fields. */
    if (GetPCIContextInfo(ctx, &ci) != PI_OK) {
        ci.size = offsetof(struct PCIInitContextInfo, bus_errors);
        GetPCIContextInfo(ctx, &ci);
    }
    if (ci.warnings & PI_WARN_NO_LEGACY_WINDOW) {
        puts("  WARNING: the ROM accessed legacy VGA memory (PCI 0xA0000-0xBFFFF),");
        puts("  which this PCI bus board cannot forward; reads returned zero and");
        puts("  writes were dropped. The original Prometheus and the G-REX have");
        puts("  this limitation; FireStorm, Firebird, Mediator and AmigaPCI do not.");
        missed = 1;
    }
    printf("  CPU stop=%lu instructions=%lu accesses=%lu fault=%08lx op=%lu INT=%lu\n", (ULONG)ci.execution.reason,
           (ULONG)ci.execution.instructions, (ULONG)ci.execution.accesses, (ULONG)ci.execution.address,
           (ULONG)ci.execution.operation, (ULONG)ci.execution.interrupt);
    if (ci.warnings & PI_WARN_NO_IO_WINDOW)
        puts(
            "  WARNING: the ROM accessed I/O ports, which this PCI bus board cannot\n"
            "  forward; reads returned all-ones and writes were dropped.");
    if (ci.blocking_card != PI_NO_BLOCKING_CARD)
        printf("  BLOCKED: the display controller at %lu:%lu.%lu decodes the VGA ports\n",
               (ULONG)(ci.blocking_card >> 8), (ULONG)((ci.blocking_card >> 3) & 31), (ULONG)(ci.blocking_card & 7));
    legacy_line(ctx, "");
    if (ci.warnings & PI_WARN_BUS_ERROR)
        printf(
            "  WARNING: %lu card access(es) ended in a 68k bus error, the first at %08lx;\n"
            "  reads returned all-ones and writes were dropped, as on a PC\n",
            (ULONG)ci.bus_errors, (ULONG)ci.bus_error_address);
    if (log_services && services.count) {
        unsigned i;
        puts("  SERVICES: missing BIOS services, answered with AH=86h and CF set:");
        for (i = 0; i < services.count; ++i)
            printf("    INT %02lXh AX=%04lXh\n", (ULONG)(service_entries[i] >> 16),
                   (ULONG)(service_entries[i] & 0xffff));
    }
    if ((ci.warnings & PI_WARN_MISSING_SERVICE) && !log_services)
        printf(
            "  MISSING: the ROM called INT %02lXh AX=%04lXh, a BIOS service neither it nor\n"
            "  pciinit.library provides; the run stopped there\n",
            (ULONG)(ci.missing_service >> 16), (ULONG)(ci.missing_service & 0xffff));
    else if (ci.execution.reason == X86_STOP_FAULT)
        printf("  FAULT: CPU exception %lu (%s)\n", (ULONG)ci.execution.interrupt,
               ci.execution.interrupt == 0    ? "divide error"
               : ci.execution.interrupt == 6  ? "invalid opcode"
               : ci.execution.interrupt == 13 ? "general protection"
                                              : "see the x86 exception vectors");
    else if (ci.execution.reason == X86_STOP_ACCESS)
        puts("  ACCESS: a memory or port access was refused; see fault= and op= above");
    else if (ci.execution.reason == X86_STOP_LIMIT || ci.execution.reason == X86_STOP_TIMEOUT)
        puts("  LIMIT: the ROM did not finish within the instruction, access or time budget");
    printf("  PC=%04x:%08lx AX=%08lx BX=%08lx CX=%08lx DX=%08lx SI=%08lx DI=%08lx\n", diag->registers.segment[X86_CS],
           (ULONG)diag->registers.eip, (ULONG)diag->registers.eax, (ULONG)diag->registers.ebx,
           (ULONG)diag->registers.ecx, (ULONG)diag->registers.edx, (ULONG)diag->registers.esi,
           (ULONG)diag->registers.edi);
    printf("  IO=%lu VGA=%lu BAR=%lu INT10=%08lx\n", (ULONG)diag->io_accesses, (ULONG)diag->vga_accesses,
           (ULONG)diag->bar_accesses, (ULONG)diag->int10_vector);
    start = diag->count > diag->capacity ? diag->count - diag->capacity : 0;
    for (n = start; n < diag->count; ++n) {
        const struct PCIInitTraceEntry *e = &diag->entries[n % diag->capacity];
        printf("  TRACE op=%lu addr=%08lx width=%lu value=%08lx\n", (ULONG)e->operation, (ULONG)e->address,
               (ULONG)e->width, (ULONG)e->value);
    }
    return missed;
}
static __attribute__((noinline)) int run_main(int argc, char **argv)
{
    struct Library *pci;
    struct pci_dev *dev = NULL;
    unsigned cards = 0, loaded = 0, initialized = 0, blocked = 0, failed = 0, ignored = 0, dumped = 0;
    struct OpromConfig config;
    const char *config_source;
    uint32_t status       = PI_OK;
    struct TagItem tags[] = {{PII_Error, (ULONG)&status}, {TAG_DONE, 0}};
    PCIInitImage *file    = NULL;
    struct PCIInitTraceEntry trace[128];
    struct PCIInitDiagnostics diag = {.size = sizeof(diag)};
    struct X86Limits limits        = {sizeof(limits), 10000000, 30000000, 300000, NULL};
    struct TagItem context_tags[]  = {{PII_Error, (ULONG)&status},     {PII_Hardware, 1},
                                      {PII_Diagnostics, (ULONG)&diag}, {PII_Limits, (ULONG)&limits},
                                      {TAG_IGNORE, (ULONG)&services},  {TAG_IGNORE, 1},
                                      {TAG_IGNORE, (ULONG)&log_hook},  {TAG_DONE, 0}};
    if (!arguments(argc, argv)) {
        puts(
            "Usage: PCIoprom [--no-post | --list-modes] [--mode number ...] [--pattern] [--trace] [--services] "
            "[--release-vga] [--log file] [--config file] [--dump file] [ROM-file]");
        puts("Modes accept decimal or 0x-prefixed hex; --pattern requires --mode.");
        puts("--dump writes the first card ROM, as read from the card, to file.");
        puts("--trace lists the last 128 I/O, VGA, BAR and interrupt accesses after each run.");
        puts("--services lets missing BIOS services fail instead of stopping, and lists them.");
        puts("--release-vga leaves each card with VGA ports and memory decode off after use.");
        puts("--log writes every card access of POST and the BIOS calls to file; runs are slower.");
        puts("Direct-color modes show a Boing Ball by default; --pattern draws test bars.");
        puts("ENV:/ENVARC:" OPROM_CONFIG_NAME " holds 'Ignore vendor:device' lines (device may be *).");
        return 20;
    }
    if (trace_accesses) {
        diag.capacity = 128;
        diag.entries  = trace;
    }
    if (log_services)
        context_tags[4].ti_Tag = PII_ServiceLog;
    if (release_vga)
        context_tags[5].ti_Tag = PII_ReleaseVGA;
    PCIInitBase = OpenLibrary((STRPTR) "pciinit.library", PCIINIT_LIBRARY_VERSION);
    if (!PCIInitBase) {
        puts("Cannot open pciinit.library 2");
        return 20;
    }
    pci = op_open(SysBase);
    if (!pci) {
        puts("Cannot open openpci.library 14");
        CloseLibrary(PCIInitBase);
        return 20;
    }
    if (log_path) {
        context_tags[6].ti_Tag = PII_AccessHook;
        access_log             = accesslog_create();
        log_file               = access_log ? fopen(log_path, "w") : NULL;
        if (!log_file || fprintf(log_file, "; PCIoprom access log\n") < 0 || accesslog_legend(log_file)) {
            printf("Cannot write %s\n", log_path);
            if (log_file)
                fclose(log_file);
            log_file = NULL;
            failed   = 1;
            goto out;
        }
    }
    config_source = oprom_config_load(&config, config_path);
    if (config_path && !config_source) {
        printf("Cannot read %s\n", config_path);
        failed = 1;
        goto out;
    }
    if (config_source)
        printf("Configuration: %s, %u ignored\n", config_source, config.ignores);
    if (config.errors)
        printf("  WARNING: %u rejected configuration line(s); first: %s\n", config.errors, config.error);
    if (rom_path) {
        file = LoadOptionROMFromFile(rom_path, tags);
        if (!file) {
            printf("File: %s\n", status_name(status));
            failed = 1;
            goto out;
        }
    }
    while ((dev = op_find(pci, dev))) {
        PCIInitImage *image = file;
        PCIInitContext *ctx;
        struct PCIInitROMInfo info = {.size = sizeof(info)};
        ++cards;
        printf("%lu:%lu.%lu %04x:%04x ", op_attr(pci, dev, PRM_BusNumber), op_attr(pci, dev, PRM_SlotNumber),
               op_attr(pci, dev, PRM_FunctionNumber), dev->vendor, dev->device);
        if (oprom_config_ignored(&config, dev->vendor, dev->device)) {
            puts("ignored by configuration");
            ++ignored;
            continue;
        }
        if (!image)
            image = LoadOptionROMFromCard(dev, tags);
        if (!image) {
            printf("%s\n", status_name(status));
            if (status != PI_NO_ROM && status != PI_UNSUPPORTED_ROM && status != PI_CARD_BUSY)
                ++failed;
            continue;
        }
        GetOptionROMInfo(image, &info);
        if (file && (info.vendor != dev->vendor || info.device != dev->device)) {
            puts("file does not match");
            continue;
        }
        ++loaded;
        printf("ROM %lu bytes, x86 image %lu+%lu\n", (unsigned long)info.source_bytes, (unsigned long)info.image_offset,
               (unsigned long)info.init_bytes);
        if (dump_path && !dumped) {
            FILE *out = fopen(dump_path, "wb");
            int ok    = out && fwrite(info.source, 1, info.source_bytes, out) == info.source_bytes;
            if (out && fclose(out))
                ok = 0;
            if (ok) {
                printf("  Dumped %lu bytes to %s\n", (unsigned long)info.source_bytes, dump_path);
                dumped = 1;
            } else {
                printf("  Cannot write %s\n", dump_path);
                ++failed;
            }
        }
        if (no_post) {
            puts("  POST: skipped");
            if (!file)
                FreeOptionROM(image);
            continue;
        }
        ctx = CreatePCIContext(dev, context_tags);
        if (ctx) {
            memset(&diag.registers, 0, sizeof(diag.registers));
            diag.count = diag.io_accesses = diag.vga_accesses = diag.bar_accesses = diag.int10_vector = 0;
            if (log_file && fprintf(log_file, "\n; card %04x:%04x at %lu:%lu.%lu\n", dev->vendor, dev->device,
                                    op_attr(pci, dev, PRM_BusNumber), op_attr(pci, dev, PRM_SlotNumber),
                                    op_attr(pci, dev, PRM_FunctionNumber)) < 0)
                log_failed = 1;
            log_mark("POST");
            status = RunOptionROM(ctx, image);
            log_flush();
            printf("  POST: %s\n", status_name(status));
            if (report(ctx, &diag))
                ++blocked;
            if (status == PI_OK) {
                ++initialized;
                status = graphics(ctx);
                log_flush();
                if (!status && (list_modes || mode_count))
                    legacy_line(ctx, " after BIOS calls");
                if (status) {
                    printf("  Graphics: %s\n", status_name(status));
                    ++failed;
                    /* The state and trace of the BIOS call that failed. */
                    report(ctx, &diag);
                }
            } else if (status == PI_UNSUPPORTED_MAPPING)
                ++blocked;
            else
                ++failed;
            if (DeletePCIContext(ctx) != PI_OK) {
                puts("  Context cleanup failed");
                ++failed;
            }
        } else {
            printf("  Context: %s\n", status_name(status));
            ++failed;
        }
        if (!file)
            FreeOptionROM(image);
    }
out:
    if (log_file) {
        if (fclose(log_file) || log_failed) {
            printf("Cannot write %s\n", log_path);
            ++failed;
        } else
            printf("Access log: %s\n", log_path);
    }
    accesslog_free(access_log);
    if (file)
        FreeOptionROM(file);
    CloseLibrary(pci);
    CloseLibrary(PCIInitBase);
    printf("PCIOPROM RESULT cards=%u loaded=%u initialized=%u blocked=%u failures=%u ignored=%u\n", cards, loaded,
           initialized, blocked, failed, ignored);
    return failed ? 20 : blocked || !loaded ? 5 : 0;
}
/* ROM execution and the drawing code run on the caller's stack, and the
 * Shell default is far smaller than they need: borrow a larger one. Nothing
 * touched between the two swaps may live on the stack, since the compiler
 * addresses locals relative to the stack pointer that the swap replaces. */
#define PCIOPROM_STACK 131072UL
static struct StackSwapStruct borrowed;
static int borrowed_argc, borrowed_result;
static char **borrowed_argv;
static __attribute__((noinline)) void on_borrowed_stack(void)
{
    StackSwap(&borrowed);
    borrowed_result = run_main(borrowed_argc, borrowed_argv);
    StackSwap(&borrowed);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct Task *task = FindTask(NULL);
    APTR stack;
    if ((ULONG)task->tc_SPUpper - (ULONG)task->tc_SPLower >= PCIOPROM_STACK)
        return run_main(argc, argv);
    /* The emulator spends its time on this stack: fast RAM first. */
    stack = AllocMem(PCIOPROM_STACK, MEMF_FAST | MEMF_PUBLIC);
    if (!stack)
        stack = AllocMem(PCIOPROM_STACK, MEMF_PUBLIC);
    if (!stack) {
        puts("Cannot allocate a 128 KiB stack");
        return 20;
    }
    borrowed.stk_Lower   = stack;
    borrowed.stk_Upper   = (ULONG)stack + PCIOPROM_STACK;
    borrowed.stk_Pointer = (APTR)borrowed.stk_Upper;
    borrowed_argc        = argc;
    borrowed_argv        = argv;
    on_borrowed_stack();
    FreeMem(stack, PCIOPROM_STACK);
    return borrowed_result;
}
