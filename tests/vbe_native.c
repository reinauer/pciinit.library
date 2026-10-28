/* Exercise the public library ABI against the emulated PCI card. */
#include <proto/exec.h>
#include <proto/pciinit.h>
#include "private/openpci_calls.h"
#include "private/pattern.h"
#include <stdio.h>
#include <string.h>
struct Library *PCIInitBase;
static unsigned checks, failures;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct Library *pci          = NULL;
    struct pci_dev *board        = NULL;
    PCIInitImage *image          = NULL;
    PCIInitContext *ctx          = NULL, *other;
    struct PCIInitVBEInfo info   = {.size = sizeof(info)};
    struct PCIInitVBEModeInfo mi = {.size = sizeof(mi)};
    struct PCIInitFramebuffer fb = {.size = sizeof(fb)}, again = {.size = sizeof(again)};
    struct PCIInitTraceEntry trace[32];
    struct PCIInitDiagnostics diag = {.size = sizeof(diag), .capacity = 32, .entries = trace};
    uint32_t error, current, command = 0, expected_command = 0, modes[2] = {0}, i, j, status;
    struct TagItem tags[] = {
        {PII_Hardware, 1}, {PII_Error, (ULONG)&error}, {PII_Diagnostics, (ULONG)&diag}, {TAG_DONE, 0}};
    int mapped  = 0;
    PCIInitBase = OpenLibrary((STRPTR) "pciinit.library", PCIINIT_LIBRARY_VERSION);
    CHECK(PCIInitBase != NULL);
    if (!PCIInitBase)
        goto out;
    pci = op_open(SysBase);
    CHECK(pci != NULL);
    if (!pci)
        goto out;
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    CHECK(board != NULL);
    if (!board)
        goto out;
    command = expected_command = op_read(pci, board, PCI_COMMAND, 2);
    image                      = LoadOptionROMFromCard(board, NULL);
    CHECK(image != NULL);
    if (!image)
        goto out;
    ctx = CreatePCIContext(board, tags);
    CHECK(ctx != NULL);
    if (!ctx)
        goto out;
    CHECK(GetVBEInfo(ctx, &info) == PI_BAD_STATE);
    CHECK(RunOptionROM(ctx, image) == PI_OK);
    if (failures)
        goto out;
    CHECK(FreeOptionROM(image) == PI_OK);
    image  = NULL;
    status = GetVBEInfo(ctx, &info);
    CHECK(status == PI_OK);
    if (status)
        goto diag;
    printf("VBE controller version=%04lx memory=%lu OEM=%s modes=%lu\n", (ULONG)info.version, (ULONG)info.memory_bytes,
           info.oem, (ULONG)info.mode_count);
    for (i = 0; i < info.mode_count; ++i) {
        status = GetVBEModeInfo(ctx, info.modes[i], &mi);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        printf("VBE mode=%03lx %lux%lux%lu pitch=%lu attr=%04lx PCI=%08lx\n", (ULONG)mi.mode, (ULONG)mi.width,
               (ULONG)mi.height, (ULONG)mi.bits_per_pixel, (ULONG)mi.pitch, (ULONG)mi.attributes,
               (ULONG)mi.framebuffer_pci);
        if (mi.mode == 0x111)
            modes[0] = mi.mode;
        if (mi.mode == 0x114)
            modes[1] = mi.mode;
    }
    CHECK(modes[0] && modes[1]);
    if (failures)
        goto out;
    CHECK(SetVBEMode(ctx, 0x1ff, PI_VBE_LINEAR) == PI_UNSUPPORTED_MODE);
    CHECK(SetVBEMode(ctx, modes[0], 0x2000) == PI_BAD_ARGUMENT);
    CHECK(MapFramebuffer(ctx, &fb) == PI_BAD_STATE);
    for (i = 0; i < 3; ++i) {
        uint32_t mode          = modes[i & 1];
        uint32_t cycle_command = (command & ~PCI_COMMAND_MASTER) | ((i & 1) ? PCI_COMMAND_MASTER : 0);
        uint32_t flags;
        status = GetVBEModeInfo(ctx, mode, &mi);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        flags            = info.version >= 0x200 && (mi.attributes & 0x80) && mi.framebuffer_pci ? PI_VBE_LINEAR : 0;
        expected_command = cycle_command;
        op_write(pci, board, PCI_COMMAND, cycle_command, 2);
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == cycle_command);
        status = SetVBEMode(ctx, mode, flags);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        CHECK(GetCurrentVBEMode(ctx, &current) == PI_OK);
        CHECK((current & 0x7fff) == (mode | flags));
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == cycle_command);
        if (!flags) {
            /* A banked mode has no flat window; the context stays usable. */
            static uint8_t out[4096], in[4096];
            uint32_t total = mi.pitch * mi.height, offsets[3] = {0, 0x10000 - 2048, 0}, k;
            CHECK(MapFramebuffer(ctx, &fb) == PI_UNSUPPORTED_MAPPING);
            CHECK(op_read(pci, board, PCI_COMMAND, 2) == cycle_command);
            /* Pixels travel through the VGA window: a row at the start, a
             * span across the first bank boundary and the final bytes. */
            offsets[2] = total - sizeof(out);
            for (k = 0; k < sizeof(out); ++k)
                out[k] = (uint8_t)(k * 7 + i);
            CHECK(WriteFramebuffer(ctx, 0, NULL, 16) == PI_BAD_ARGUMENT);
            CHECK(WriteFramebuffer(ctx, total, out, 1) == PI_BAD_ARGUMENT);
            CHECK(ReadFramebuffer(ctx, 0, in, 0) == PI_BAD_ARGUMENT);
            for (k = 0; k < 3; ++k) {
                status = WriteFramebuffer(ctx, offsets[k], out, sizeof(out));
                CHECK(status == PI_OK);
                if (status)
                    goto diag;
            }
            for (k = 0; k < 3; ++k) {
                memset(in, 0, sizeof(in));
                status = ReadFramebuffer(ctx, offsets[k], in, sizeof(in));
                CHECK(status == PI_OK);
                if (status)
                    goto diag;
                CHECK(!memcmp(in, out, sizeof(in)));
            }
            CHECK(op_read(pci, board, PCI_COMMAND, 2) == cycle_command);
            CHECK(GetCurrentVBEMode(ctx, &current) == PI_OK && (current & 0x7fff) == mode);
            status = SetVBEMode(ctx, mode, PI_VBE_KEEP_MEMORY);
            CHECK(status == PI_OK);
            if (status)
                goto diag;
            printf("VBE cycle=%lu mode=%03lx %lux%lux%lu pitch=%lu banked, no framebuffer mapping\n", (ULONG)i,
                   (ULONG)mode, (ULONG)mi.width, (ULONG)mi.height, (ULONG)mi.bits_per_pixel, (ULONG)mi.pitch);
            fflush(stdout);
            op_write(pci, board, PCI_COMMAND, command, 2);
            expected_command = command;
            continue;
        }
        status = MapFramebuffer(ctx, &fb);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        mapped = 1;
        CHECK(!(op_read(pci, board, PCI_COMMAND, 2) & PCI_COMMAND_MASTER));
        CHECK(fb.address != NULL && fb.bytes == fb.mode.pitch * fb.mode.height);
        CHECK(fb.mode.mode == mode && (fb.flags & PI_FB_LITTLE_ENDIAN));
        CHECK(MapFramebuffer(ctx, &again) == PI_BAD_STATE);
        CHECK(GetVBEInfo(ctx, &info) == PI_BAD_STATE);
        CHECK(GetVBEModeInfo(ctx, mode, &mi) == PI_BAD_STATE);
        CHECK(GetCurrentVBEMode(ctx, &current) == PI_BAD_STATE);
        CHECK(SetVBEMode(ctx, mode, flags) == PI_BAD_STATE);
        CHECK(DeletePCIContext(ctx) == PI_BAD_STATE);
        other = CreatePCIContext(board, tags);
        CHECK(other == NULL && error == PI_BAD_STATE);
        CHECK(LoadOptionROMFromCard(board, tags) == NULL && error == PI_BAD_STATE);
        CHECK(draw_pattern(&fb));
        CHECK(UnmapFramebuffer(ctx) == PI_OK);
        mapped = 0;
        CHECK(UnmapFramebuffer(ctx) == PI_BAD_STATE);
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == cycle_command);
        CHECK(MapFramebuffer(ctx, &fb) == PI_OK);
        mapped = 1;
        CHECK(draw_pattern(&fb));
        CHECK(UnmapFramebuffer(ctx) == PI_OK);
        mapped = 0;
        /* A preserve-memory mode set must retain visible VRAM. Use the
         * same mode so a layout change cannot hide an unwanted clear. */
        status = MapFramebuffer(ctx, &fb);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        mapped                                              = 1;
        ((volatile uint8_t *)fb.address)[0]                 = 0x5a;
        ((volatile uint8_t *)fb.address)[fb.mode.pitch + 3] = 0xa5;
        ((volatile uint8_t *)fb.address)[fb.bytes - 1]      = 0x3c;
        CHECK(UnmapFramebuffer(ctx) == PI_OK);
        mapped = 0;
        status = SetVBEMode(ctx, mode, flags | PI_VBE_KEEP_MEMORY);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        CHECK(GetCurrentVBEMode(ctx, &current) == PI_OK && (current & 0x7fff) == (mode | flags));
        status = MapFramebuffer(ctx, &fb);
        CHECK(status == PI_OK);
        if (status)
            goto diag;
        mapped = 1;
        CHECK(((volatile uint8_t *)fb.address)[0] == 0x5a);
        CHECK(((volatile uint8_t *)fb.address)[fb.mode.pitch + 3] == 0xa5);
        CHECK(((volatile uint8_t *)fb.address)[fb.bytes - 1] == 0x3c);
        CHECK(draw_pattern(&fb));
        CHECK(UnmapFramebuffer(ctx) == PI_OK);
        mapped = 0;
        printf("VBE cycle=%lu mode=%03lx %lux%lux%lu pitch=%lu bytes=%lu PCI=%08lx CPU=%08lx pattern verified\n",
               (ULONG)i, (ULONG)mode, (ULONG)fb.mode.width, (ULONG)fb.mode.height, (ULONG)fb.mode.bits_per_pixel,
               (ULONG)fb.mode.pitch, (ULONG)fb.bytes, (ULONG)fb.mode.framebuffer_pci, (ULONG)fb.address);
        fflush(stdout);
        op_write(pci, board, PCI_COMMAND, command, 2);
        expected_command = command;
    }
    goto out;
diag:
    printf("VBE failure status=%lu PC=%04x:%08lx AX=%08lx\n", (ULONG)status, diag.registers.segment[X86_CS],
           (ULONG)diag.registers.eip, (ULONG)diag.registers.eax);
    for (j = diag.count > diag.capacity ? diag.count - diag.capacity : 0; j < diag.count; ++j) {
        struct PCIInitTraceEntry *e = &trace[j % diag.capacity];
        printf("TRACE op=%lu addr=%08lx width=%lu value=%08lx\n", (ULONG)e->operation, (ULONG)e->address,
               (ULONG)e->width, (ULONG)e->value);
    }
out:
    if (mapped)
        CHECK(UnmapFramebuffer(ctx) == PI_OK);
    if (ctx) {
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == expected_command);
        op_write(pci, board, PCI_COMMAND, command, 2);
    }
    if (ctx)
        CHECK(DeletePCIContext(ctx) == PI_OK);
    if (image)
        CHECK(FreeOptionROM(image) == PI_OK);
    if (board && pci) {
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == command);
        CHECK(op_obtain(pci, board));
        op_release(pci, board);
    }
    if (pci)
        CloseLibrary(pci);
    if (PCIInitBase)
        CloseLibrary(PCIInitBase);
    printf("VBE RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
