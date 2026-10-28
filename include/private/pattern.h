#ifndef PCIINIT_PATTERN_H
#define PCIINIT_PATTERN_H
#include <libraries/pciinit.h>
/* Pixel bytes follow VBE little-endian storage, independent of CPU order. */
static uint32_t pattern_pixel(const struct PCIInitVBEModeInfo *m, uint32_t x, uint32_t y)
{
    unsigned bar = x * 8 / m->width;
    uint32_t r = (bar & 4) ? 255 : 0, g = (bar & 2) ? 255 : 0, b = (bar & 1) ? 255 : 0;
    if (y >= m->height / 2) {
        r = r * (m->height - y) * 2 / m->height;
        g = g * (m->height - y) * 2 / m->height;
        b = b * (m->height - y) * 2 / m->height;
    }
    return ((r * ((1U << m->red_size) - 1) / 255) << m->red_position) |
           ((g * ((1U << m->green_size) - 1) / 255) << m->green_position) |
           ((b * ((1U << m->blue_size) - 1) / 255) << m->blue_position);
}
static int draw_pattern(const struct PCIInitFramebuffer *fb)
{
    const struct PCIInitVBEModeInfo *m = &fb->mode;
    uint32_t x, y, k, bytes = (m->bits_per_pixel + 7) / 8;
    if (m->memory_model != 6 || !(fb->flags & PI_FB_LITTLE_ENDIAN))
        return 0;
    for (y = 0; y < m->height; ++y)
        for (x = 0; x < m->width; ++x) {
            uint32_t pixel      = pattern_pixel(m, x, y);
            volatile uint8_t *p = fb->address + y * m->pitch + x * bytes;
            for (k = 0; k < bytes; ++k)
                p[k] = pixel >> (8 * k);
        }
    /* Sample each color bar and gradient, including the last scanline. */
    for (y = 0; y < 3; ++y)
        for (x = 0; x < 8; ++x) {
            uint32_t px = (2 * x + 1) * m->width / 16, py = y == 2 ? m->height - 1 : y * m->height / 2;
            uint32_t pixel      = pattern_pixel(m, px, py);
            volatile uint8_t *p = fb->address + py * m->pitch + px * bytes;
            for (k = 0; k < bytes; ++k)
                if (p[k] != (uint8_t)(pixel >> (8 * k)))
                    return 0;
        }
    return 1;
}
#endif
