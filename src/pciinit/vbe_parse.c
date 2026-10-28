#include "private/vbe.h"
#include <string.h>
static uint32_t word(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t dword(const uint8_t *p)
{
    return word(p) | (word(p + 2) << 16);
}
static int readable(uint32_t a, uint32_t n)
{
    return a < 0x100000 && n <= 0x100000 - a && !(a < 0xc0000 && a + n > 0xa0000);
}
static uint32_t far_address(const uint8_t *p)
{
    return (word(p + 2) << 4) + word(p);
}
uint32_t pi_vbe_parse_info(const uint8_t *ram, struct PCIInitVBEInfo *out)
{
    const uint8_t *p           = ram + PI_VBE_BUFFER;
    struct PCIInitVBEInfo info = {.size = sizeof(info)};
    uint32_t address, i, mode;
    if (memcmp(p, "VESA", 4) || word(p + 4) < 0x100)
        return PI_BAD_ROM;
    info.version      = word(p + 4);
    info.capabilities = dword(p + 10);
    info.memory_bytes = word(p + 18) << 16;
    address           = far_address(p + 6);
    if (!address)
        return PI_BAD_ROM;
    for (i = 0; i < sizeof(info.oem) - 1; ++i) {
        if (!readable(address + i, 1))
            return PI_BAD_ROM;
        info.oem[i] = ram[address + i];
        if (!info.oem[i])
            break;
    }
    address = far_address(p + 14);
    if (!address)
        return PI_BAD_ROM;
    for (i = 0; i <= PI_VBE_MAX_MODES; ++i) {
        if (!readable(address + 2 * i, 2))
            return PI_BAD_ROM;
        mode = word(ram + address + 2 * i);
        if (mode == 0xffff) {
            info.mode_count = i;
            *out            = info;
            return PI_OK;
        }
        /* S3 BIOSes list OEM modes above 1FFh; only the flag bits are invalid. */
        if (i == PI_VBE_MAX_MODES || (mode & ~0x3fffU))
            return PI_BAD_ROM;
        info.modes[i] = mode;
    }
    return PI_BAD_ROM;
}
uint32_t pi_vbe_parse_mode(const uint8_t *ram, uint32_t version, uint32_t mode, struct PCIInitVBEModeInfo *out)
{
    const uint8_t *p            = ram + PI_VBE_BUFFER;
    unsigned color              = 31;
    struct PCIInitVBEModeInfo m = {.size = sizeof(m), .mode = mode};
    m.attributes                = word(p);
    m.width                     = word(p + 18);
    m.height                    = word(p + 20);
    m.pitch                     = word(p + 16);
    m.planes                    = p[24];
    m.bits_per_pixel            = p[25];
    m.memory_model              = p[27];
    m.image_pages               = p[29];
    m.framebuffer_pci           = dword(p + 40);
    if (version >= 0x300 && (m.attributes & 0x80)) {
        m.pitch       = word(p + 50);
        m.image_pages = p[53];
        color         = 54;
    }
    m.red_size          = p[color];
    m.red_position      = p[color + 1];
    m.green_size        = p[color + 2];
    m.green_position    = p[color + 3];
    m.blue_size         = p[color + 4];
    m.blue_position     = p[color + 5];
    m.reserved_size     = p[color + 6];
    m.reserved_position = p[color + 7];
    *out                = m;
    return PI_OK;
}
uint32_t pi_vbe_layout(const struct PCIInitVBEModeInfo *m, uint32_t memory)
{
    uint32_t bytes = (m->bits_per_pixel + 7) / 8, used = 0, i;
    const uint32_t sizes[]     = {m->red_size, m->green_size, m->blue_size, m->reserved_size};
    const uint32_t positions[] = {m->red_position, m->green_position, m->blue_position, m->reserved_position};
    if ((m->attributes & 0x11) != 0x11 || m->planes != 1 ||
        !((m->memory_model == 4 && m->bits_per_pixel == 8) ||
          (m->memory_model == 6 &&
           (m->bits_per_pixel == 15 || m->bits_per_pixel == 16 || m->bits_per_pixel == 24 || m->bits_per_pixel == 32))))
        return PI_UNSUPPORTED_MODE;
    if (!m->width || !m->height || m->width > 0xffffffffU / bytes || m->pitch < m->width * bytes ||
        m->height > 0xffffffffU / m->pitch || m->pitch * m->height > memory)
        return PI_BAD_ROM;
    if (m->memory_model == 6)
        for (i = 0; i < 4; ++i) {
            uint32_t mask, bits = i == 3 ? bytes * 8 : m->bits_per_pixel;
            if (!sizes[i]) {
                if (i < 3)
                    return PI_BAD_ROM;
                continue;
            }
            if (sizes[i] > 8 || positions[i] >= bits || sizes[i] > bits - positions[i])
                return PI_BAD_ROM;
            mask = ((1U << sizes[i]) - 1) << positions[i];
            if (mask & used)
                return PI_BAD_ROM;
            used |= mask;
        }
    return PI_OK;
}
