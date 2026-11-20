/* Inject failure only in this executable's allocator, never production code. */
#include "private/pciinit.h"
#include "private/engine.h"
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures, live;
static int remaining = -1;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
void *pi_alloc(size_t bytes)
{
    void *p;
    if (remaining == 0)
        return NULL;
    if (remaining > 0)
        --remaining;
    p = calloc(1, bytes);
    if (p)
        ++live;
    return p;
}
void pi_free(void *p)
{
    if (p) {
        --live;
        free(p);
    }
}
static uint32_t X86_CALLBACK memory(void *user, uint32_t a, uint32_t w, uint32_t op, uint32_t *v)
{
    (void)user;
    (void)a;
    (void)w;
    (void)op;
    (void)v;
    return 1;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct PIBase base       = {0};
    struct X86Config config  = {.size = sizeof(config), .version = X86EMU_ABI_VERSION, .access = memory};
    struct TagItem offline[] = {{PII_Offline, 1}, {TAG_DONE, 0}};
    unsigned char rom[512]   = {0};
    unsigned i, sum = 0;
    FILE *file;
    const char *path      = "ram:pciinit-alloc.rom";
    struct Library *pci   = NULL;
    struct pci_dev *board = NULL;
    base.sys              = pi_sysbase();
    InitSemaphore(&base.vga_lock);
    base.engine = OpenLibrary((STRPTR) "x86emu.library", 1);
    CHECK(base.engine != NULL);
    if (!base.engine)
        return 20;
    remaining = 0;
    CHECK(xe_create(&config) == NULL);
    CHECK(live == 0);
    remaining = -1;
    for (i = 0; i < 2; ++i) {
        remaining = i;
        CHECK(pi_create(&base, NULL, offline) == NULL);
        CHECK(live == 0 && base.objects == 0);
    }
    remaining = -1;
    rom[0]    = 0x55;
    rom[1]    = 0xaa;
    rom[2]    = 1;
    rom[3]    = 0xcb;
    rom[0x18] = 0x20;
    memcpy(rom + 0x20, "PCIR", 4);
    rom[0x2a] = 24;
    rom[0x30] = 1;
    rom[0x35] = 0x80;
    for (i = 0; i < 511; ++i)
        sum += rom[i];
    rom[511] = (unsigned char)-sum;
    file     = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        CHECK(fwrite(rom, 1, sizeof(rom), file) == sizeof(rom));
        CHECK(fclose(file) == 0);
        for (i = 0; i < 2; ++i) {
            remaining = i;
            CHECK(pi_load_file(&base, path, NULL) == NULL);
            CHECK(live == 0 && base.objects == 0);
        }
        remaining = -1;
        {
            PCIInitImage *image = pi_load_file(&base, path, NULL);
            CHECK(image != NULL);
            if (image)
                CHECK(pi_free_image(&base, image) == PI_OK);
            CHECK(live == 0 && base.objects == 0);
        }
    }
    if (argc > 1) {
        pci = op_open(SysBase);
        CHECK(pci != NULL);
        if (pci) {
            while ((board = op_find(pci, board)))
                if ((op_read(pci, board, 8, 4) >> 24) == 3)
                    break;
            CHECK(board != NULL);
        }
        if (board)
            for (i = 0; i < 2; ++i) {
                remaining = i;
                CHECK(pi_create(&base, board, NULL) == NULL);
                CHECK(live == 0 && base.objects == 0);
                CHECK(op_obtain(pci, board));
                op_release(pci, board);
                remaining = i;
                CHECK(pi_load_card(&base, board, NULL) == NULL);
                CHECK(live == 0 && base.objects == 0);
                CHECK(op_obtain(pci, board));
                op_release(pci, board);
            }
        if (pci)
            CloseLibrary(pci);
    }
    remaining = -1;
    remove(path);
    CloseLibrary(base.engine);
    CHECK(live == 0 && base.objects == 0);
    printf("FOOTPRINT pci_context=%lu image=%lu library_base=%lu guest_ram=%lu\n",
           (unsigned long)sizeof(PCIInitContext), (unsigned long)sizeof(PCIInitImage),
           (unsigned long)sizeof(struct PIBase), (unsigned long)PI_RAM_BYTES);
    printf("ALLOCATION RESULT checks=%u failures=%u live=%u\n", checks, failures, live);
    (void)argv;
    return failures ? 20 : 0;
}
