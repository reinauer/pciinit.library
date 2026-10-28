/* CRC-32 reference vectors and bounded image patching. */
#include "private/quirks.h"
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
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    static uint8_t image[1024];
    static const struct PIPatch list[] = {{0, 1, 0xa5}, {0x10, 2, 0x1234}, {0x20, 4, 0x89abcdef}, {1020, 4, 0x11223344},
                                          {1021, 4, 0}, {5000, 1, 0},      {0x30, 3, 0},          {0, 0, 0}};
    const struct PIQuirk *q;
    unsigned i;
    CHECK(pi_crc32((const uint8_t *)"", 0) == 0);
    CHECK(pi_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926UL);
    CHECK(pi_crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43) == 0x414fa339UL);
    memset(image, 0x5a, sizeof(image));
    CHECK(pi_apply_patches(image, sizeof(image), list) == 4);
    CHECK(image[0] == 0xa5 && image[1] == 0x5a);
    CHECK(image[0x10] == 0x34 && image[0x11] == 0x12 && image[0x12] == 0x5a);
    CHECK(image[0x20] == 0xef && image[0x21] == 0xcd && image[0x22] == 0xab && image[0x23] == 0x89);
    CHECK(image[1020] == 0x44 && image[1023] == 0x11);
    CHECK(image[0x30] == 0x5a && image[0x31] == 0x5a);
    CHECK(pi_apply_patches(image, sizeof(image), NULL) == 0);
    CHECK(pi_find_quirk(0, 0) == NULL);
    q = pi_find_quirk(0xb412d266UL, 32768);
    CHECK(q && q->patches[0].offset == 0x43 && q->patches[0].width == 1 && q->patches[0].value == 9 && q->note);
    CHECK(pi_find_quirk(0xb412d266UL, 65536) == NULL);
    /* Every table entry patches inside its image. */
    for (i = 0; q && q->patches[i].width; ++i)
        CHECK(q->patches[i].offset + q->patches[i].width <= q->bytes);
    printf("QUIRKS RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
