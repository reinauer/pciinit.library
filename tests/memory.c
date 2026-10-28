#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
static uint8_t source[128], a[128], b[128];
#define CHECK(x)        \
    do {                \
        ++checks;       \
        if (!(x))       \
            ++failures; \
    } while (0)
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    for (unsigned so = 0; so < 8; ++so)
        for (unsigned dest = 0; dest < 8; ++dest)
            for (unsigned n = 0; n < 100; ++n) {
                for (unsigned i = 0; i < 128; ++i)
                    source[i] = i * 37 + 11, a[i] = b[i] = 0xa5;
                for (unsigned i = 0; i < n; ++i)
                    b[dest + i] = source[so + i];
                CHECK(memcpy(a + dest, source + so, n) == a + dest);
                for (unsigned i = 0; i < 128; ++i)
                    CHECK(a[i] == b[i]);
                CHECK(!memcmp(a + dest, source + so, n));
                if (n) {
                    unsigned at = n / 2;
                    a[dest + at] ^= 0xff;
                    int expected = (int)a[dest + at] - (int)source[so + at];
                    int result   = memcmp(a + dest, source + so, n);
                    CHECK((result > 0) - (result < 0) == (expected > 0) - (expected < 0));
                }
                CHECK(memset(a + dest, 0x193, n) == a + dest);
                for (unsigned i = 0; i < 128; ++i)
                    CHECK(a[i] == (i >= dest && i < dest + n ? 0x93 : 0xa5));
            }
    {
        /* Large fills take the MOVEM path from every alignment. */
        static uint8_t big[8192 + 128];
        static const unsigned lengths[] = {63, 64, 65, 100, 1000, 4097, 8192};
        for (unsigned dest = 0; dest < 8; ++dest)
            for (unsigned l = 0; l < 7; ++l) {
                unsigned n = lengths[l];
                for (unsigned i = 0; i < sizeof(big); ++i)
                    big[i] = 0xa5;
                CHECK(memset(big + dest, 0x5c, n) == big + dest);
                for (unsigned i = 0; i < sizeof(big); ++i)
                    CHECK(big[i] == (i >= dest && i < dest + n ? 0x5c : 0xa5));
            }
    }
    printf("MEMORY checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
