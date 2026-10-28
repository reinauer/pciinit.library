#include "private/quirks.h"
#include <stddef.h>

/* S3 86C325 VBE 1.2 ROM (32 KiB init image). Its POST reads the
 * configuration byte at offset 0x43: bit 0 prints the banner and bit 3
 * skips the 1.75-second PIT channel-2 banner hold, together with the
 * text-screen clear and cursor home which follow it. Setting bit 3
 * removes the hold; nothing else in the POST reads the byte. */
static const struct PIPatch s3_86c325_banner[] = {{0x43, 1, 0x09}, {0, 0, 0}};

static const struct PIQuirk quirks[] = {
    {0xb412d266UL, 32768, s3_86c325_banner, "S3 86C325 VBE 1.2: skip banner hold"},
};

/* Reflected CRC-32, polynomial 0xEDB88320, as zlib.crc32 computes it.
 * A nibble table keeps the library small; images are at most 64 KiB. */
uint32_t pi_crc32(const uint8_t *data, uint32_t bytes)
{
    static const uint32_t table[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                       0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                       0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    uint32_t crc                    = 0xffffffffUL;
    while (bytes--) {
        crc ^= *data++;
        crc = table[crc & 15] ^ (crc >> 4);
        crc = table[crc & 15] ^ (crc >> 4);
    }
    return ~crc;
}
const struct PIQuirk *pi_find_quirk(uint32_t crc, uint32_t bytes)
{
    unsigned i;
    for (i = 0; i < sizeof(quirks) / sizeof(quirks[0]); ++i)
        if (quirks[i].crc == crc && quirks[i].bytes == bytes)
            return &quirks[i];
    return NULL;
}
uint32_t pi_apply_patches(uint8_t *image, uint32_t bytes, const struct PIPatch *patches)
{
    uint32_t applied = 0;
    for (; patches && patches->width; ++patches) {
        uint32_t i;
        if ((patches->width != 1 && patches->width != 2 && patches->width != 4) || patches->offset >= bytes ||
            patches->width > bytes - patches->offset)
            continue;
        for (i = 0; i < patches->width; ++i)
            image[patches->offset + i] = patches->value >> (8 * i);
        ++applied;
    }
    return applied;
}
