/* A synthetic ROM installs an INT 10h handler and retains its current mode. */
#include <proto/exec.h>
#include <proto/pciinit.h>
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
static unsigned char rom[2048];
static const char path[] = "ram:pciinit-vbe.rom";
static void w(unsigned a, unsigned v)
{
    rom[a]     = v;
    rom[a + 1] = v >> 8;
}
static void fixture(unsigned version, unsigned fault)
{
    /* AX=4F00/01 copies a block from CS:SI to the caller's ES:DI. */
    static const unsigned char copy[] = {0x1e, 0x0e, 0x1f, 0xbe, 0,    2,    0xb9, 0,
                                         2,    0xf3, 0xa4, 0x1f, 0xb8, 0x4f, 0,    0xcf};
    static const unsigned char set[]  = {0x2e, 0x89, 0x1e, 0, 6, 0xb8, 0x4f, 0, 0xcf};
    static const unsigned char get[]  = {0x2e, 0x8b, 0x1e, 0, 6, 0xb8, 0x4f, 0, 0xcf};
    static const unsigned char post[] = {0xc7, 0x06, 0x40, 0, 0, 1, 0xc7, 0x06, 0x42, 0, 0, 0xc0, 0xcb};
    unsigned i, sum = 0;
    FILE *file;
    memset(rom, 0, sizeof(rom));
    rom[0] = 0x55;
    rom[1] = 0xaa;
    rom[2] = 4;
    memcpy(rom + 3, post, sizeof(post));
    rom[0x18] = 0x40;
    memcpy(rom + 0x40, "PCIR", 4);
    w(0x44, 0x5333);
    w(0x46, 0x5631);
    w(0x4a, 24);
    w(0x50, 4);
    rom[0x55] = 0x80;
    for (i = 0; i < 4; ++i) {
        unsigned p = 0x100 + 8 * i, target = 0x140 + 32 * i;
        rom[p] = 0x3d;
        w(p + 1, 0x4f00 + i); /* cmp ax,function */
        rom[p + 3] = 0x75;
        rom[p + 4] = 3; /* jne next */
        rom[p + 5] = 0xe9;
        w(p + 6, target - (p + 8));
    }
    rom[0x120] = 0xb8;
    w(0x121, 0x14f);
    rom[0x123] = 0xcf;
    memcpy(rom + 0x140, copy, sizeof(copy));
    memcpy(rom + 0x160, copy, sizeof(copy));
    w(0x164, 0x400);
    w(0x167, 0x100);
    memcpy(rom + 0x180, set, sizeof(set));
    memcpy(rom + 0x1a0, get, sizeof(get));
    memcpy(rom + 0x200, "VESA", 4);
    w(0x204, version);
    w(0x206, 0x640);
    w(0x208, 0xc000);
    w(0x20e, 0x610);
    w(0x210, 0xc000);
    w(0x212, 64);
    w(0x400, version < 0x200 ? 0x11 : 0x91);
    w(0x410, 1280);
    w(0x412, 640);
    w(0x414, 480);
    rom[0x418] = 1;
    rom[0x419] = 16;
    rom[0x41b] = 6;
    rom[0x41f] = 5;
    rom[0x420] = 11;
    rom[0x421] = 6;
    rom[0x422] = 5;
    rom[0x423] = 5;
    w(0x42a, 0x1c00); /* PhysBasePtr = 1C000000 */
    w(0x600, 3);
    w(0x610, 0x111);
    w(0x612, 0x114);
    w(0x614, 0xffff);
    memcpy(rom + 0x640, "Synthetic VBE", 14);
    if (fault == 1) {
        rom[0x140] = 0xb8;
        w(0x141, 0x14f);
        rom[0x143] = 0xcf;
    }
    if (fault == 2) {
        rom[0x140] = 0xeb;
        rom[0x141] = 0xfe;
    }
    if (fault == 3)
        rom[0x140] = 0xf4;
    if (fault == 4) {
        w(0x206, 0xffff);
        w(0x208, 0xffff);
    }
    if (fault == 5)
        rom[0x180] = 0xcf; /* AX is never replaced with success. */
    if (fault == 6)
        memset(rom + 0x180, 0x90, 5); /* Success without changing mode. */
    for (i = 0; i < sizeof(rom) - 1; ++i)
        sum += rom[i];
    rom[sizeof(rom) - 1] = -sum;
    file                 = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        CHECK(fwrite(rom, 1, sizeof(rom), file) == sizeof(rom));
        CHECK(fclose(file) == 0);
    }
}
static void run_case(unsigned version, unsigned fault)
{
    volatile uint32_t cancel     = 0;
    struct X86Limits limits      = {sizeof(limits), 10000, 100000, 1000, &cancel};
    struct TagItem tags[]        = {{PII_Offline, 1}, {PII_Limits, (ULONG)&limits}, {TAG_DONE, 0}};
    struct PCIInitVBEInfo info   = {.size = sizeof(info)}, old;
    struct PCIInitVBEModeInfo mi = {.size = sizeof(mi)};
    struct PCIInitContextInfo ci = {.size = sizeof(ci)};
    struct PCIInitFramebuffer fb = {.size = sizeof(fb)};
    PCIInitImage *image;
    PCIInitContext *ctx;
    uint32_t mode = 0xdeadbeef, status, flags = version >= 0x200 ? PI_VBE_LINEAR : 0;
    unsigned i;
    fixture(version, fault);
    image = LoadOptionROMFromFile(path, NULL);
    CHECK(image != NULL);
    ctx = CreatePCIContext(NULL, tags);
    CHECK(ctx != NULL);
    if (!ctx || !image)
        goto out;
    CHECK(GetVBEInfo(ctx, &info) == PI_BAD_STATE);
    CHECK(GetCurrentVBEMode(ctx, &mode) == PI_BAD_STATE && mode == 0xdeadbeef);
    CHECK(RunOptionROM(ctx, image) == PI_OK);
    CHECK(FreeOptionROM(image) == PI_OK);
    image  = NULL;
    old    = info;
    status = GetVBEInfo(ctx, &info);
    if (fault >= 1 && fault <= 4) {
        CHECK(status == (fault == 1 ? PI_VBE_FAILED : fault == 4 ? PI_BAD_ROM : PI_EXECUTION_FAILED));
        CHECK(!memcmp(&info, &old, sizeof(info)));
        CHECK(GetPCIContextInfo(ctx, &ci) == PI_OK);
        CHECK(ci.state == ((fault == 2 || fault == 3) ? PI_FAILED : PI_INITIALIZED));
        if (fault == 2)
            CHECK(ci.execution.reason == X86_STOP_LIMIT);
        if (fault == 3)
            CHECK(ci.execution.reason == X86_STOP_HALT);
        if (fault == 2 || fault == 3)
            CHECK(GetCurrentVBEMode(ctx, &mode) == PI_BAD_STATE);
        goto out;
    }
    CHECK(status == PI_OK && info.version == version && info.mode_count == 2);
    CHECK(!strcmp(info.oem, "Synthetic VBE"));
    CHECK(GetVBEModeInfo(ctx, 0x111, &mi) == PI_OK);
    CHECK(mi.width == 640 && mi.height == 480 && mi.pitch == 1280 && mi.red_position == 11);
    CHECK(GetCurrentVBEMode(ctx, &mode) == PI_OK && mode == 3);
    if (fault == 5 || fault == 6) {
        CHECK(SetVBEMode(ctx, 0x111, flags) == PI_VBE_FAILED);
        CHECK(MapFramebuffer(ctx, &fb) == PI_BAD_STATE);
        CHECK(GetCurrentVBEMode(ctx, &mode) == PI_OK && mode == 3);
        goto out;
    }
    CHECK(GetVBEInfo(ctx, NULL) == PI_BAD_ARGUMENT);
    --info.size;
    CHECK(GetVBEInfo(ctx, &info) == PI_BAD_ARGUMENT);
    ++info.size;
    CHECK(GetVBEModeInfo(ctx, 0x4111, &mi) == PI_BAD_ARGUMENT);
    CHECK(GetCurrentVBEMode(ctx, NULL) == PI_BAD_ARGUMENT);
    CHECK(SetVBEMode(ctx, 0x111, 0x2000) == PI_BAD_ARGUMENT);
    CHECK(SetVBEMode(ctx, 0x1ff, flags) == PI_UNSUPPORTED_MODE);
    CHECK(MapFramebuffer(ctx, &fb) == PI_BAD_STATE);
    if (version < 0x200)
        CHECK(SetVBEMode(ctx, 0x111, PI_VBE_LINEAR) == PI_UNSUPPORTED_MODE);
    for (i = 0; i < 3; ++i) {
        unsigned selected = i == 1 ? 0x114 : 0x111;
        CHECK(SetVBEMode(ctx, selected, flags | PI_VBE_KEEP_MEMORY) == PI_OK);
        CHECK(GetCurrentVBEMode(ctx, &mode) == PI_OK && mode == (selected | flags | PI_VBE_KEEP_MEMORY));
        CHECK(GetVBEInfo(ctx, &info) == PI_OK);
    }
    CHECK(MapFramebuffer(ctx, &fb) == PI_BAD_STATE);         /* Offline has no PCI mapping. */
    CHECK(WriteFramebuffer(ctx, 0, &fb, 1) == PI_BAD_STATE); /* nor a VGA window */
    CHECK(UnmapFramebuffer(ctx) == PI_BAD_STATE);
    cancel = 1;
    CHECK(GetCurrentVBEMode(ctx, &mode) == PI_EXECUTION_FAILED);
    CHECK(GetPCIContextInfo(ctx, &ci) == PI_OK && ci.execution.reason == X86_STOP_CANCEL);
    CHECK(GetVBEInfo(ctx, &info) == PI_BAD_STATE);
out:
    if (ctx)
        CHECK(DeletePCIContext(ctx) == PI_OK);
    if (image)
        CHECK(FreeOptionROM(image) == PI_OK);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    unsigned i;
    PCIInitBase = OpenLibrary((STRPTR) "pciinit.library", PCIINIT_LIBRARY_VERSION);
    CHECK(PCIInitBase != NULL);
    if (!PCIInitBase)
        return 20;
    run_case(0x102, 0);
    run_case(0x200, 0);
    for (i = 1; i <= 6; ++i)
        run_case(0x200, i);
    CHECK(UnmapFramebuffer(NULL) == PI_BAD_ARGUMENT);
    remove(path);
    RemLibrary(PCIInitBase);
    CloseLibrary(PCIInitBase);
    printf("VBE API RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
