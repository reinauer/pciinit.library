/* Tilted red/white Boing Ball, based on the supplied fixed-point renderer. */
#include "private/boing.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void boing_release(struct BoingBall *b)
{
    free(b->latitude);
    memset(b, 0, sizeof(*b));
}
int boing_prepare(struct BoingBall *b, uint32_t width, uint32_t height)
{
    const double pi = 3.141592653589793;
    uint32_t r, i;
    int32_t *tables;
    /* This also bounds every signed coordinate/product in the renderer. */
    if (!width || !height || width > 32767 || height > 32767)
        return 0;
    r = height * 2 / 5; /* diameter is approximately 80% of screen height */
    if (r > (width - 1) / 2)
        r = (width - 1) / 2;
    if (r > (height - 1) / 2)
        r = (height - 1) / 2;
    tables = malloc((5 * r + 3) * sizeof(*tables));
    if (!tables)
        return 0;
    boing_release(b);
    b->width     = width;
    b->height    = height;
    b->radius    = r;
    b->latitude  = tables;
    b->longitude = tables + 2 * r + 1;
    b->inverse   = tables + 4 * r + 2;
    if (!r) {
        tables[0] = tables[1] = 0;
        tables[2]             = 65536;
        return 1;
    }
    for (i = 0; i <= 2 * r; ++i) {
        double t        = ((double)i - r) / r;
        int32_t lat     = (int32_t)((asin(t) + pi / 2) / pi * 65536);
        b->latitude[i]  = lat > 65535 ? 65535 : lat;
        b->longitude[i] = (int32_t)(acos(t) / (2 * pi) * 65536);
    }
    for (i = 0; i <= r; ++i) {
        double half = sqrt((double)r * r - (double)i * i);
        if (half < 1)
            half = 1;
        b->inverse[i] = (int32_t)((double)r / half * 65536);
    }
    return 1;
}
static uint32_t packed(const struct PCIInitVBEModeInfo *m, uint32_t rgb)
{
    return (((rgb >> 16 & 255) * ((1U << m->red_size) - 1) / 255) << m->red_position) |
           (((rgb >> 8 & 255) * ((1U << m->green_size) - 1) / 255) << m->green_position) |
           (((rgb & 255) * ((1U << m->blue_size) - 1) / 255) << m->blue_position);
}
static void pixel(volatile uint8_t *p, uint32_t value, unsigned bytes)
{
    unsigned i;
    for (i = 0; i < bytes; ++i)
        p[i] = value >> (8 * i);
}
int boing_draw(const struct BoingBall *b, const struct PCIInitFramebuffer *fb, uint32_t phase)
{
    const struct PCIInitVBEModeInfo *m = &fb->mode;
    unsigned bytes                     = (m->bits_per_pixel + 7) / 8;
    uint32_t x, y, gray, red, white, used = 0, i;
    const uint32_t sizes[]  = {m->red_size, m->green_size, m->blue_size};
    const uint32_t shifts[] = {m->red_position, m->green_position, m->blue_position};
    int r = b->radius, cx = b->width / 2, cy = b->height / 2, dx, dy;
    if (!b->latitude || !fb->address || m->memory_model != 6 || !(fb->flags & PI_FB_LITTLE_ENDIAN) ||
        (m->bits_per_pixel != 15 && m->bits_per_pixel != 16 && m->bits_per_pixel != 24 && m->bits_per_pixel != 32) ||
        m->width != b->width || m->height != b->height || m->pitch < m->width * bytes ||
        m->height > UINT32_MAX / m->pitch || fb->bytes < m->pitch * m->height)
        return 0;
    for (i = 0; i < 3; ++i) {
        uint32_t mask;
        if (!sizes[i] || sizes[i] > 8 || shifts[i] >= m->bits_per_pixel || sizes[i] > m->bits_per_pixel - shifts[i])
            return 0;
        mask = ((1U << sizes[i]) - 1) << shifts[i];
        if (mask & used)
            return 0;
        used |= mask;
    }
    gray  = packed(m, 0xaaaaaa);
    red   = packed(m, 0xff0000);
    white = packed(m, 0xffffff);
    for (y = 0; y < m->height; ++y)
        for (x = 0; x < m->width; ++x)
            pixel(fb->address + y * m->pitch + x * bytes, gray, bytes);
    if (!r) {
        pixel(fb->address + cy * m->pitch + cx * bytes, red, bytes);
        return 1;
    }
    for (dy = -r; dy <= r; ++dy) {
        /* Increment the rotation along each row. Constants are 16.16;
         * this is the same 20-degree tilt without per-pixel tilt multiplies. */
        int32_t rx = -r * 61583 - dy * 22414, ry = -r * 22414 + dy * 61583;
        for (dx = -r; dx <= r; ++dx, rx += 61583, ry += 22414) {
            int ri, ay, sx, lat, lon;
            int64_t stretched;
            if (dx * dx + dy * dy > r * r)
                continue;
            ri = ry >> 16;
            if (ri < -r)
                ri = -r;
            if (ri > r)
                ri = r;
            ay  = ri < 0 ? -ri : ri;
            lat = (b->latitude[ri + r] * 8) >> 16;
            /* Clamp before narrowing, including large-radius pole rows. */
            stretched = ((int64_t)rx * b->inverse[ay]) >> 32;
            sx        = stretched < -r ? 0 : stretched > r ? 2 * r : (int)stretched + r;
            lon       = (((uint32_t)b->longitude[sx] + phase) & 65535) * 16 >> 16;
            pixel(fb->address + (cy + dy) * m->pitch + (cx + dx) * bytes, ((lat + lon) & 1) ? white : red, bytes);
        }
    }
    return 1;
}
