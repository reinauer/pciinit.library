#include "private/rom.h"
#include <string.h>
static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}
uint32_t pi_rom_parse(const uint8_t *data, uint32_t bytes, struct PCIInitROMInfo *info)
{
    struct PCIInitROMInfo selected = {.size = sizeof(selected)};
    uint32_t offset = 0, count = 0, found = 0;
    if (!data || !info || info->size != sizeof(*info))
        return PI_BAD_ARGUMENT;
    if (bytes < 512 || bytes > PCIINIT_MAX_ROM)
        return PI_BAD_ROM;
    for (;;) {
        const uint8_t *image, *pcir;
        uint32_t ptr, length, init, i, sum = 0, structure;
        if (++count > 64 || bytes - offset < 512)
            return PI_BAD_ROM;
        image = data + offset;
        if (image[0] != 0x55 || image[1] != 0xaa)
            return PI_BAD_ROM;
        ptr = le16(image + 0x18);
        if (ptr < 0x1a || ptr > bytes - offset - 24)
            return PI_BAD_ROM;
        pcir = image + ptr;
        if (memcmp(pcir, "PCIR", 4))
            return PI_BAD_ROM;
        length    = le16(pcir + 0x10) * 512;
        structure = le16(pcir + 0x0a);
        if (!length || length > bytes - offset || structure < 24 || ptr > length || structure > length - ptr)
            return PI_BAD_ROM;
        if (pcir[0x14] == 0) {
            init = image[2] * 512;
            if (!init || init > length || ptr + structure > init)
                return PI_BAD_ROM;
            for (i = 0; i < init; ++i)
                sum += image[i];
            if (sum & 255)
                return PI_BAD_ROM;
            if (!found) {
                selected.source_bytes = bytes;
                selected.image_offset = offset;
                selected.image_bytes  = length;
                selected.init_bytes   = init;
                selected.vendor       = le16(pcir + 4);
                selected.device       = le16(pcir + 6);
                found                 = 1;
            }
        }
        if (pcir[0x15] & 0x80)
            break;
        offset += length;
    }
    if (!found)
        return PI_UNSUPPORTED_ROM;
    selected.image_count = count;
    *info                = selected;
    return PI_OK;
}
