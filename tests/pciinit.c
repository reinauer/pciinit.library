#include <proto/exec.h>
#include <proto/pciinit.h>
#include "private/openpci_calls.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
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
static const char path[] = "ram:pciinit-test.rom";
static uint32_t error;
static struct TagItem loadtags[] = {{PII_Error, (ULONG)&error}, {TAG_DONE, 0}};
static void make_rom(const unsigned char *code, unsigned bytes)
{
    unsigned char rom[512] = {0};
    unsigned i, sum = 0;
    FILE *file;
    rom[0] = 0x55;
    rom[1] = 0xaa;
    rom[2] = 1;
    memcpy(rom + 3, code, bytes);
    rom[0x18] = 0x40;
    memcpy(rom + 0x40, "PCIR", 4);
    rom[0x44] = 0x33;
    rom[0x45] = 0x53;
    rom[0x46] = 0x31;
    rom[0x47] = 0x56;
    rom[0x4a] = 24;
    rom[0x50] = 1;
    rom[0x55] = 0x80;
    /* ROM-installed interrupt handler: mov ax,beef; iret. */
    rom[0x80] = 0xb8;
    rom[0x81] = 0xef;
    rom[0x82] = 0xbe;
    rom[0x83] = 0xcf;
    for (i = 0; i < 511; ++i)
        sum += rom[i];
    rom[511] = (unsigned char)-sum;
    file     = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        CHECK(fwrite(rom, 1, sizeof(rom), file) == sizeof(rom));
        CHECK(fclose(file) == 0);
    }
}
static void execute_case(const unsigned char *code, unsigned bytes, unsigned expected, unsigned reason,
                         volatile uint32_t *cancel)
{
    PCIInitImage *image;
    PCIInitContext *ctx;
    struct X86Limits limits = {sizeof(limits), 1000, 10000, 1000, cancel};
    struct TagItem tags[] = {{PII_Offline, 1}, {PII_Limits, (ULONG)&limits}, {PII_Error, (ULONG)&error}, {TAG_DONE, 0}};
    struct PCIInitContextInfo info = {.size = sizeof(info)};
    make_rom(code, bytes);
    image = LoadOptionROMFromFile(path, loadtags);
    CHECK(image != NULL && error == PI_OK);
    ctx = CreatePCIContext(NULL, tags);
    CHECK(ctx != NULL && error == PI_OK);
    if (ctx && image) {
        CHECK(RunOptionROM(ctx, image) == expected);
        CHECK(GetPCIContextInfo(ctx, &info) == PI_OK);
        CHECK(info.state == (expected == PI_OK ? PI_INITIALIZED : PI_FAILED));
        CHECK(info.status == expected && info.execution.reason == reason);
        CHECK(RunOptionROM(ctx, image) == PI_BAD_STATE);
        /* The context owns its shadow and remains valid without the image. */
        CHECK(FreeOptionROM(image) == PI_OK);
        image = NULL;
        CHECK(GetPCIContextInfo(ctx, &info) == PI_OK);
    }
    if (image)
        CHECK(FreeOptionROM(image) == PI_OK);
    if (ctx)
        CHECK(DeletePCIContext(ctx) == PI_OK);
}
/* A service nobody provides stops the run and is named in the info. */
static void missing_service_case(void)
{
    static const unsigned char code[] = {0xb8, 0x65, 0xf9, 0xcd, 0x15, 0xcb};
    PCIInitImage *image;
    PCIInitContext *ctx;
    struct TagItem tags[]          = {{PII_Offline, 1}, {PII_Error, (ULONG)&error}, {TAG_DONE, 0}};
    struct PCIInitContextInfo info = {.size = sizeof(info)};
    make_rom(code, sizeof(code));
    image = LoadOptionROMFromFile(path, loadtags);
    CHECK(image != NULL);
    ctx = CreatePCIContext(NULL, tags);
    CHECK(ctx != NULL);
    if (ctx && image) {
        CHECK(RunOptionROM(ctx, image) == PI_EXECUTION_FAILED);
        CHECK(GetPCIContextInfo(ctx, &info) == PI_OK);
        CHECK(info.execution.reason == X86_STOP_FAULT && info.execution.interrupt == 0x15);
        CHECK((info.warnings & PI_WARN_MISSING_SERVICE) && info.missing_service == 0x0015f965UL);
    }
    if (image)
        CHECK(FreeOptionROM(image) == PI_OK);
    if (ctx)
        CHECK(DeletePCIContext(ctx) == PI_OK);
}
/* Only the current structure size is accepted. */
static void context_info_size_case(void)
{
    struct PCIInitContextInfo info = {.size = sizeof(info) - sizeof(uint32_t)};
    struct TagItem tags[]          = {{PII_Offline, 1}, {TAG_DONE, 0}};
    PCIInitContext *ctx            = CreatePCIContext(NULL, tags);
    CHECK(ctx != NULL);
    if (!ctx)
        return;
    CHECK(GetPCIContextInfo(ctx, &info) == PI_BAD_ARGUMENT && info.state == 0);
    info.size = sizeof(info);
    CHECK(GetPCIContextInfo(ctx, &info) == PI_OK);
    CHECK(info.size == sizeof(info) && info.state == PI_CREATED && info.offline == 1 && info.warnings == 0);
    CHECK(DeletePCIContext(ctx) == PI_OK);
}
static void card_case(const char *dump)
{
    struct Library *pci = op_open(SysBase);
    struct pci_dev *dev = NULL;
    PCIInitImage *image, *file;
    PCIInitContext *ctx, *busy;
    struct PCIInitROMInfo a = {.size = sizeof(a)}, b = {.size = sizeof(b)};
    uint32_t command, rom;
    CHECK(pci != NULL);
    if (!pci)
        return;
    while ((dev = op_find(pci, dev)))
        if ((op_read(pci, dev, 8, 4) >> 24) == 3)
            break;
    CHECK(dev != NULL);
    if (!dev) {
        CloseLibrary(pci);
        return;
    }
    command = op_read(pci, dev, PCI_COMMAND, 2);
    rom     = op_read(pci, dev, PCI_ROM_ADDRESS, 4);
    image   = LoadOptionROMFromCard(dev, loadtags);
    CHECK(image != NULL && error == PI_OK);
    CHECK(op_read(pci, dev, PCI_COMMAND, 2) == command);
    CHECK(op_read(pci, dev, PCI_ROM_ADDRESS, 4) == rom);
    file = LoadOptionROMFromFile(dump, loadtags);
    CHECK(file != NULL && error == PI_OK);
    if (image && file) {
        CHECK(GetOptionROMInfo(image, &a) == PI_OK);
        CHECK(GetOptionROMInfo(file, &b) == PI_OK);
        /* Same metadata; the loaded bytes live in separate copies. */
        CHECK(memcmp(&a, &b, offsetof(struct PCIInitROMInfo, source)) == 0);
        CHECK(a.source && b.source && a.source != b.source && memcmp(a.source, b.source, a.source_bytes) == 0);
        printf("CARD ROM vendor=%04x device=%04x source=%lu init=%lu\n", a.vendor, a.device,
               (unsigned long)a.source_bytes, (unsigned long)a.init_bytes);
    }
    ctx = CreatePCIContext(dev, loadtags);
    CHECK(ctx != NULL && error == PI_OK);
    if (ctx) {
        busy = CreatePCIContext(dev, loadtags);
        CHECK(busy == NULL && error == PI_CARD_BUSY);
        if (busy)
            DeletePCIContext(busy);
        CHECK(LoadOptionROMFromCard(dev, loadtags) == NULL && error == PI_CARD_BUSY);
        if (image)
            CHECK(RunOptionROM(ctx, image) == PI_UNSUPPORTED_MAPPING);
        CHECK(DeletePCIContext(ctx) == PI_OK);
    }
    CHECK(op_obtain(pci, dev));
    op_release(pci, dev);
    CHECK(op_read(pci, dev, PCI_COMMAND, 2) == command);
    CHECK(op_read(pci, dev, PCI_ROM_ADDRESS, 4) == rom);
    if (file)
        FreeOptionROM(file);
    if (image)
        FreeOptionROM(image);
    CloseLibrary(pci);
}
static void diagnostic_case(void)
{
    static const unsigned char bios[] = {0xcd, 0x12, 0xcb};
    struct PCIInitTraceEntry entry;
    struct PCIInitDiagnostics d = {.size = sizeof(d), .capacity = 1, .entries = &entry};
    struct TagItem tags[] = {{PII_Offline, 1}, {PII_Diagnostics, (ULONG)&d}, {PII_Error, (ULONG)&error}, {TAG_DONE, 0}};
    PCIInitContext *ctx;
    PCIInitImage *image;
    --d.size;
    CHECK(CreatePCIContext(NULL, tags) == NULL && error == PI_BAD_ARGUMENT);
    ++d.size;
    d.capacity = 4097;
    CHECK(CreatePCIContext(NULL, tags) == NULL && error == PI_BAD_ARGUMENT);
    d.capacity = 1;
    d.entries  = NULL;
    CHECK(CreatePCIContext(NULL, tags) == NULL && error == PI_BAD_ARGUMENT);
    d.entries = &entry;
    CHECK(LoadOptionROMFromFile(path, tags) == NULL && error == PI_BAD_ARGUMENT);
    make_rom(bios, sizeof(bios));
    image = LoadOptionROMFromFile(path, loadtags);
    CHECK(image != NULL);
    ctx = CreatePCIContext(NULL, tags);
    CHECK(ctx != NULL);
    if (ctx && image) {
        CHECK(RunOptionROM(ctx, image) == PI_OK);
        CHECK(d.count == 1 && entry.operation == PI_TRACE_INTERRUPT && entry.address == 0x12);
        CHECK(d.registers.eax == 640 && d.registers.segment[X86_CS] == 0 && d.registers.eip == 0x7001);
        CHECK(d.io_accesses == 0 && d.vga_accesses == 0 && d.bar_accesses == 0 && d.int10_vector == 0);
    }
    if (ctx)
        CHECK(DeletePCIContext(ctx) == PI_OK);
    if (image)
        CHECK(FreeOptionROM(image) == PI_OK);
    tags[1].ti_Tag  = PII_Hardware;
    tags[1].ti_Data = 1;
    CHECK(CreatePCIContext(NULL, tags) == NULL && error == PI_BAD_ARGUMENT);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct Library *again;
    struct TagItem cycle[2]          = {{PII_Error, (ULONG)&error}, {TAG_MORE, 0}};
    struct TagItem invalid[]         = {{PII_Error, (ULONG)&error}, {0x81234567, 1}, {TAG_DONE, 0}};
    volatile uint32_t cancel         = 1;
    static const unsigned char ret[] = {0xcb}, halt[] = {0xf4}, loop[] = {0xeb, 0xfe};
    static const unsigned char io[]  = {0xe4, 0x80, 0xcb};
    static const unsigned char vga[] = {0xb8, 0x00, 0xa0, 0x8e, 0xd8, 0xa0, 0, 0, 0xcb};
    /* Install INT 10h in IVT, call it, verify result, then RETF. */
    static const unsigned char vector[] = {0xc7, 0x06, 0x40, 0,    0x80, 0,    0xc7, 0x06, 0x42, 0,   0,
                                           0xc0, 0xcd, 0x10, 0x3d, 0xef, 0xbe, 0x75, 1,    0xcb, 0xf4};
    static const unsigned char bios[]   = {0xcd, 0x12, 0x3d, 0x80, 2, 0x75, 1, 0xcb, 0xf4};
    /* Point INT 42h at the system BIOS video entry F000:F065 as Cirrus ROMs
     * do and call it; then RETF. The code must stay below the PCIR pointer. */
    static const unsigned char system[] = {0xc7, 0x06, 0x08, 0x01, 0x65, 0xf0, 0xc7, 0x06,
                                           0x0a, 0x01, 0x00, 0xf0, 0xcd, 0x42, 0xcb};
    /* The AT model byte at F000:FFFE. */
    static const unsigned char model[] = {0xb8, 0x00, 0xf0, 0x8e, 0xd8, 0x80, 0x3e,
                                          0xfe, 0xff, 0xfc, 0x75, 1,    0xcb, 0xf4};
    PCIInitBase                        = OpenLibrary((STRPTR) "pciinit.library", 1);
    CHECK(PCIInitBase != NULL);
    if (!PCIInitBase)
        return 20;
    CHECK(PCIInitBase->lib_NegSize == 120);
    again = OpenLibrary((STRPTR) "pciinit.library", 1);
    CHECK(again == PCIInitBase);
    if (again)
        CloseLibrary(again);
    CHECK(LoadOptionROMFromFile("ram:nonexistent-pciinit.rom", loadtags) == NULL && error == PI_IO_ERROR);
    CHECK(LoadOptionROMFromFile(NULL, loadtags) == NULL && error == PI_BAD_ARGUMENT);
    CHECK(CreatePCIContext(NULL, loadtags) == NULL && error == PI_BAD_ARGUMENT);
    CHECK(LoadOptionROMFromFile(path, invalid) == NULL && error == PI_BAD_ARGUMENT);
    cycle[1].ti_Data = (ULONG)cycle;
    CHECK(LoadOptionROMFromFile(path, cycle) == NULL && error == PI_BAD_ARGUMENT);
    CHECK(FreeOptionROM(NULL) == PI_BAD_ARGUMENT);
    CHECK(DeletePCIContext(NULL) == PI_BAD_ARGUMENT);
    execute_case(ret, sizeof(ret), PI_OK, X86_STOP_HALT, NULL);
    execute_case(vector, sizeof(vector), PI_OK, X86_STOP_HALT, NULL);
    execute_case(bios, sizeof(bios), PI_OK, X86_STOP_HALT, NULL);
    execute_case(system, sizeof(system), PI_OK, X86_STOP_HALT, NULL);
    execute_case(model, sizeof(model), PI_OK, X86_STOP_HALT, NULL);
    execute_case(halt, sizeof(halt), PI_EXECUTION_FAILED, X86_STOP_HALT, NULL);
    execute_case(loop, sizeof(loop), PI_EXECUTION_FAILED, X86_STOP_LIMIT, NULL);
    execute_case(io, sizeof(io), PI_EXECUTION_FAILED, X86_STOP_ACCESS, NULL);
    execute_case(vga, sizeof(vga), PI_EXECUTION_FAILED, X86_STOP_ACCESS, NULL);
    execute_case(ret, sizeof(ret), PI_EXECUTION_FAILED, X86_STOP_CANCEL, &cancel);
    diagnostic_case();
    missing_service_case();
    context_info_size_case();
    if (argc > 1)
        card_case(argv[1]);
    remove(path);
    RemLibrary(PCIInitBase);
    CHECK(PCIInitBase->lib_Flags & LIBF_DELEXP);
    CloseLibrary(PCIInitBase);
    PCIInitBase = OpenLibrary((STRPTR) "pciinit.library", 1);
    CHECK(PCIInitBase != NULL);
    if (PCIInitBase) {
        RemLibrary(PCIInitBase);
        CloseLibrary(PCIInitBase);
    }
    printf("PCIINIT RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
