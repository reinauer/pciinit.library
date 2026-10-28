#include "private/boing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(x)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #x); \
        }                                          \
    } while (0)
static struct PCIInitFramebuffer frame(unsigned width, unsigned height, unsigned bits)
{
    struct PCIInitFramebuffer f = {.size = sizeof(f), .flags = PI_FB_LITTLE_ENDIAN};
    f.mode.width                = width;
    f.mode.height               = height;
    f.mode.bits_per_pixel       = bits;
    f.mode.memory_model         = 6;
    f.mode.pitch                = width * ((bits + 7) / 8) + 7;
    f.mode.red_size = f.mode.blue_size = bits < 24 ? 5 : 8;
    f.mode.green_size                  = bits == 16 ? 6 : f.mode.red_size;
    f.mode.green_position              = f.mode.blue_size;
    f.mode.red_position                = f.mode.blue_size + f.mode.green_size;
    f.bytes                            = f.mode.pitch * height;
    return f;
}
static unsigned read_pixel(const struct PCIInitFramebuffer *f, unsigned x, unsigned y)
{
    unsigned v = 0, i, bytes = (f->mode.bits_per_pixel + 7) / 8;
    for (i = 0; i < bytes; ++i)
        v |= (unsigned)f->address[y * f->mode.pitch + x * bytes + i] << (i * 8);
    return v;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    const unsigned bits[]   = {15, 16, 24, 32};
    const unsigned reds[]   = {0x7c00, 0xf800, 0xff0000, 0xff0000};
    const unsigned whites[] = {0x7fff, 0xffff, 0xffffff, 0xffffff};
    const unsigned grays[]  = {0x5294, 0xa554, 0xaaaaaa, 0xaaaaaa};
    struct BoingBall b      = {0};
    unsigned n, x, y, white, red;
    CHECK(!boing_prepare(&b, 0, 80));
    CHECK(!boing_prepare(&b, 32768, 80));
    for (n = 0; n < 4; ++n) {
        struct PCIInitFramebuffer f = frame(96, 80, bits[n]);
        uint8_t *storage            = malloc(f.bytes + 32);
        unsigned bytes              = (bits[n] + 7) / 8;
        CHECK(storage != NULL);
        if (!storage)
            return 20;
        memset(storage, 0x5a, f.bytes + 32);
        f.address = storage + 16;
        CHECK(boing_prepare(&b, 96, 80));
        CHECK(b.radius == 32);
        CHECK(boing_draw(&b, &f, 0));
        white = red = 0;
        for (y = 0; y < 80; ++y)
            for (x = 0; x < 96; ++x) {
                unsigned v = read_pixel(&f, x, y);
                CHECK(v == reds[n] || v == whites[n] || v == grays[n]);
                red += v == reds[n];
                white += v == whites[n];
            }
        CHECK(red > 1000 && white > 1000 && red + white == 3209);
        CHECK(read_pixel(&f, 0, 0) == grays[n]);
        for (y = 0; y < 80; ++y)
            for (x = 96 * bytes; x < f.mode.pitch; ++x)
                CHECK(f.address[y * f.mode.pitch + x] == 0x5a);
        for (x = 0; x < 16; ++x)
            CHECK(storage[x] == 0x5a && storage[16 + f.bytes + x] == 0x5a);
        /* One longitude cell swaps red and white, preserving the silhouette. */
        CHECK(boing_draw(&b, &f, 4096));
        {
            unsigned changed = 0;
            for (y = 0; y < 80; ++y)
                for (x = 0; x < 96; ++x)
                    changed += read_pixel(&f, x, y) == reds[n];
            CHECK(changed == white);
        }
        f.flags = 0;
        CHECK(!boing_draw(&b, &f, 0));
        f.flags = PI_FB_LITTLE_ENDIAN;
        f.bytes--;
        CHECK(!boing_draw(&b, &f, 0));
        f.bytes++;
        f.mode.pitch = 0;
        CHECK(!boing_draw(&b, &f, 0));
        boing_release(&b);
        free(storage);
    }
    CHECK(boing_prepare(&b, 30, 80));
    CHECK(b.radius == 14);
    boing_release(&b);
    CHECK(boing_prepare(&b, 1, 1));
    CHECK(b.radius == 0);
    boing_release(&b);
    if (argc == 2) {
        struct PCIInitFramebuffer f = frame(640, 480, 32);
        FILE *out                   = fopen(argv[1], "wb");
        f.address                   = malloc(f.bytes);
        CHECK(out && f.address && boing_prepare(&b, 640, 480));
        CHECK(b.radius == 192);
        CHECK(boing_draw(&b, &f, 0));
        if (out && f.address) {
            fprintf(out, "P6\n640 480\n255\n");
            for (y = 0; y < 480; ++y)
                for (x = 0; x < 640; ++x) {
                    unsigned v    = read_pixel(&f, x, y);
                    uint8_t rgb[] = {v >> 16, v >> 8, v};
                    if (fwrite(rgb, 1, 3, out) != 3)
                        return 20;
                }
            CHECK(fclose(out) == 0);
        }
        boing_release(&b);
        free((void *)f.address);
    }
    printf("BOING RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
