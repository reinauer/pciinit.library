#include "private/rom.h"
#include <stdio.h>
#include <string.h>
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
static void checksum(unsigned char *p)
{
    unsigned i, sum = 0;
    p[511] = 0;
    for (i = 0; i < 512; ++i)
        sum += p[i];
    p[511] = (unsigned char)-sum;
}
static void image(unsigned char *p, unsigned type, unsigned last)
{
    memset(p, 0, 512);
    p[0]    = 0x55;
    p[1]    = 0xaa;
    p[2]    = 1;
    p[3]    = 0xcb;
    p[0x18] = 0x20;
    memcpy(p + 0x20, "PCIR", 4);
    p[0x24] = 0x33;
    p[0x25] = 0x53;
    p[0x26] = 0x31;
    p[0x27] = 0x56;
    p[0x2a] = 24;
    p[0x30] = 1;
    p[0x34] = type;
    p[0x35] = last ? 0x80 : 0;
    checksum(p);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct PCIInitROMInfo info = {.size = sizeof(info)};
    unsigned n;
    image(rom, 0, 1);
    CHECK(pi_rom_parse(rom, 512, &info) == PI_OK);
    CHECK(info.vendor == 0x5333 && info.device == 0x5631);
    CHECK(info.init_bytes == 512 && info.image_bytes == 512 && info.image_count == 1);
    CHECK(pi_rom_parse(NULL, 512, &info) == PI_BAD_ARGUMENT);
    CHECK(pi_rom_parse(rom, 512, NULL) == PI_BAD_ARGUMENT);
    info.size = 0;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ARGUMENT);
    info.size = sizeof(info);
    for (n = 0; n < 512; ++n)
        CHECK(pi_rom_parse(rom, n, &info) == PI_BAD_ROM);
    CHECK(pi_rom_parse(rom, PCIINIT_MAX_ROM + 1, &info) == PI_BAD_ROM);
    rom[10] ^= 1;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    rom[10] ^= 1;
    rom[0x18] = 0xff;
    rom[0x19] = 0xff;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 1);
    rom[0x30] = 0;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 1);
    rom[0x30] = 2;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 1);
    rom[0x2a] = 0;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 1);
    rom[2] = 2;
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 1);
    rom[0x20] = 'Q';
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 3, 1);
    CHECK(pi_rom_parse(rom, 512, &info) == PI_UNSUPPORTED_ROM);
    image(rom, 3, 0);
    image(rom + 512, 0, 1);
    CHECK(pi_rom_parse(rom, 1024, &info) == PI_OK);
    CHECK(info.image_offset == 512 && info.image_count == 2);
    rom[512] = 0;
    CHECK(pi_rom_parse(rom, 1024, &info) == PI_BAD_ROM);
    image(rom, 0, 0);
    CHECK(pi_rom_parse(rom, 512, &info) == PI_BAD_ROM);
    image(rom, 0, 0);
    image(rom + 512, 3, 1);
    CHECK(pi_rom_parse(rom, 1024, &info) == PI_OK);
    CHECK(info.image_offset == 0 && info.image_count == 2);
    /* Reject a malformed trailing image even after selecting valid x86. */
    rom[512 + 0x2a] = 0;
    CHECK(pi_rom_parse(rom, 1024, &info) == PI_BAD_ROM);
    printf("ROM RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
