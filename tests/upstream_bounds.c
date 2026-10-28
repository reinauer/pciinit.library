/* Standalone libx86emu API reproducer; no Amiga wrapper is involved. */
#include <x86emu.h>
#include <stdio.h>
#include <string.h>
static unsigned char code[64];
static unsigned writes, fetches;
static unsigned access_memory(x86emu_t *emu, unsigned address, unsigned *value, unsigned type)
{
    unsigned i, width = 1U << (type & 255), operation = type & ~255U;
    if (operation == X86EMU_MEMIO_W) {
        ++writes;
        x86emu_stop(emu);
        return 0;
    }
    *value = 0;
    if (operation == X86EMU_MEMIO_X) {
        ++fetches;
        for (i = 0; i < width; ++i) {
            if (address + i < 0x1000 || address + i >= 0x1000 + sizeof(code)) {
                x86emu_stop(emu);
                return 1;
            }
            *value |= (unsigned)code[address + i - 0x1000] << (8 * i);
        }
    }
    return 0;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    x86emu_t *emu   = x86emu_new(0, 0);
    unsigned prefix = argc > 1 && !strcmp(argv[1], "prefix");
    unsigned passed;
    if (prefix) {
        memset(code, 0xf3, 32);
        code[32] = 0xf4;
    } else {
        const unsigned char rep[] = {0xb9, 64, 0, 0xf3, 0xaa, 0xf4};
        memcpy(code, rep, sizeof(rep));
    }
    x86emu_set_seg_register(emu, emu->x86.seg + R_CS_INDEX, 0);
    emu->x86.R_EIP = 0x1000;
    x86emu_set_memio_handler(emu, access_memory);
    x86emu_run(emu, 0);
    passed = prefix ? fetches == 15 : writes == 1;
    printf("%s writes=%u fetches=%u result=%s\n", prefix ? "prefix" : "rep", writes, fetches, passed ? "PASS" : "FAIL");
    x86emu_done(emu);
    return passed ? 0 : 1;
}
