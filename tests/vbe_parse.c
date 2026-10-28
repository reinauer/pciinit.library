#include "private/vbe.h"
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
static unsigned char ram[0x100000];
static void w(unsigned a, unsigned v)
{
    ram[a]     = v;
    ram[a + 1] = v >> 8;
}
static void d(unsigned a, unsigned v)
{
    w(a, v);
    w(a + 2, v >> 16);
}
static void controller(void)
{
    memset(ram, 0, sizeof(ram));
    memcpy(ram + PI_VBE_BUFFER, "VESA", 4);
    w(PI_VBE_BUFFER + 4, 0x200);
    d(PI_VBE_BUFFER + 6, 0xc0000100);
    d(PI_VBE_BUFFER + 14, 0xc0000200);
    w(PI_VBE_BUFFER + 18, 64);
    memcpy(ram + 0xc0100, "Test ROM", 9);
    w(0xc0200, 0x111);
    w(0xc0202, 0x114);
    w(0xc0204, 0xffff);
}
static void mode(void)
{
    unsigned p = PI_VBE_BUFFER;
    memset(ram + p, 0, 256);
    w(p, 0x91);
    w(p + 16, 1280);
    w(p + 18, 640);
    w(p + 20, 480);
    ram[p + 24] = 1;
    ram[p + 25] = 16;
    ram[p + 27] = 6;
    ram[p + 31] = 5;
    ram[p + 32] = 11;
    ram[p + 33] = 6;
    ram[p + 34] = 5;
    ram[p + 35] = 5;
    d(p + 40, 0x1c000000);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct PCIInitVBEInfo info  = {.size = sizeof(info)}, saved;
    struct PCIInitVBEModeInfo m = {.size = sizeof(m)}, good;
    unsigned i, p = PI_VBE_BUFFER;
    controller();
    CHECK(pi_vbe_parse_info(ram, &info) == PI_OK);
    CHECK(info.version == 0x200 && info.memory_bytes == 0x400000 && info.mode_count == 2);
    CHECK(!strcmp(info.oem, "Test ROM") && info.modes[0] == 0x111 && info.modes[1] == 0x114);
    saved  = info;
    ram[p] = 'X';
    CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    CHECK(!memcmp(&info, &saved, sizeof(info)));
    controller();
    w(p + 4, 0xff);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    for (i = 0; i < 2; ++i) {
        unsigned field = p + (i ? 14 : 6);
        controller();
        d(field, 0);
        CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
        controller();
        d(field, 0xa0000000);
        CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
        controller();
        d(field, 0xffffffff);
        CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
        controller();
        d(field, 0x9fff000f);
        ram[0x9ffff] = 1;
        CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    }
    controller();
    for (i = 0; i <= PI_VBE_MAX_MODES; ++i)
        w(0xc0200 + 2 * i, 0x111);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    w(0xc0200 + 2 * PI_VBE_MAX_MODES, 0xffff);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_OK);
    CHECK(info.mode_count == PI_VBE_MAX_MODES);
    controller();
    w(0xc0200, 0x4111);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    /* An S3 Trio64 BIOS lists OEM modes 202h-213h after the VESA ones. */
    controller();
    w(0xc0202, 0x213);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_OK && info.modes[1] == 0x213);
    controller();
    w(0xc0202, 0x8111);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_BAD_ROM);
    controller();
    memset(ram + 0xc0100, 'A', 128);
    CHECK(pi_vbe_parse_info(ram, &info) == PI_OK);
    CHECK(info.oem[126] == 'A' && !info.oem[127]);
    mode();
    CHECK(pi_vbe_parse_mode(ram, 0x200, 0x111, &m) == PI_OK);
    CHECK(m.pitch == 1280 && m.width == 640 && m.height == 480 && m.framebuffer_pci == 0x1c000000);
    CHECK(m.red_size == 5 && m.red_position == 11 && m.green_size == 6 && m.blue_position == 0);
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_OK);
    good    = m;
    m.pitch = 0;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_BAD_ROM);
    m       = good;
    m.pitch = 1279;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_BAD_ROM);
    m        = good;
    m.height = 0;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_BAD_ROM);
    m       = good;
    m.width = 0x80000000;
    CHECK(pi_vbe_layout(&m, 0xffffffff) == PI_BAD_ROM);
    m        = good;
    m.height = 0xffffffff;
    CHECK(pi_vbe_layout(&m, 0xffffffff) == PI_BAD_ROM);
    m = good;
    CHECK(pi_vbe_layout(&m, 1280 * 480 - 1) == PI_BAD_ROM);
    CHECK(pi_vbe_layout(&m, 1280 * 480) == PI_OK);
    m        = good;
    m.planes = 4;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_UNSUPPORTED_MODE);
    m            = good;
    m.attributes = 0x90;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_UNSUPPORTED_MODE);
    m            = good;
    m.attributes = 0x81;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_UNSUPPORTED_MODE);
    m                 = good;
    m.attributes      = 0x11;
    m.framebuffer_pci = 0;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_OK);
    for (i = 0; i < 33; ++i) {
        m              = good;
        m.red_position = i;
        CHECK(pi_vbe_layout(&m, 0x400000) == (i == 11 ? PI_OK : PI_BAD_ROM));
    }
    m          = good;
    m.red_size = 0;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_BAD_ROM);
    m               = good;
    m.reserved_size = 1;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_BAD_ROM);
    m                   = good;
    m.bits_per_pixel    = 15;
    m.red_position      = 10;
    m.green_size        = 5;
    m.reserved_size     = 1;
    m.reserved_position = 15;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_OK);
    m                = good;
    m.bits_per_pixel = 8;
    m.memory_model   = 4;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_OK);
    m.memory_model = 3;
    CHECK(pi_vbe_layout(&m, 0x400000) == PI_UNSUPPORTED_MODE);
    mode();
    w(p + 50, 2560);
    ram[p + 53] = 2;
    ram[p + 54] = 8;
    ram[p + 55] = 16;
    ram[p + 56] = 8;
    ram[p + 57] = 8;
    ram[p + 58] = 8;
    ram[p + 60] = 8;
    ram[p + 61] = 24;
    ram[p + 25] = 32;
    CHECK(pi_vbe_parse_mode(ram, 0x300, 0x112, &m) == PI_OK);
    CHECK(m.pitch == 2560 && m.image_pages == 2 && m.red_size == 8 && m.red_position == 16);
    CHECK(m.reserved_size == 8 && m.reserved_position == 24 && pi_vbe_layout(&m, 0x400000) == PI_OK);
    printf("VBE PARSE RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
