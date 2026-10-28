/* Compare native 68020 division with the portable full-range arithmetic. */
#include "private/pit.h"
#include <stdio.h>
#include <stdlib.h>
static uint32_t seed = 0x5396bc1;
static uint32_t random_word(void)
{
    seed = seed * 1664525 + 1013904223;
    return seed;
}
static unsigned checks, failures;
static void check(uint64_t ticks, uint32_t hz, uint32_t reload)
{
    uint32_t remainder;
    volatile uint64_t input = ticks; /* Keep the reference as general division. */
    uint64_t quotient       = pi_pit_divmod(ticks, hz, &remainder);
    ++checks;
    if (quotient != input / hz || remainder != input % hz ||
        pi_pit_ticks(ticks, hz) != (input / hz) * 1193182 + (input % hz) * 1193182 / hz ||
        pi_pit_elapsed(ticks, reload) != input % reload)
        ++failures;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    const uint32_t rates[]  = {1, 709379, 715909, 1193182, UINT32_MAX};
    const uint64_t clocks[] = {0, 1, UINT32_MAX, UINT64_C(0x100000000), UINT64_MAX};
    unsigned repeats        = argc > 1 ? atoi(argv[1]) : 1;
    if (!repeats || repeats > 100)
        return 20;
    for (unsigned n = 0; n < repeats; ++n) {
        for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
            for (unsigned j = 0; j < sizeof(clocks) / sizeof(clocks[0]); ++j)
                check(clocks[j], rates[i], 65536);
            /* Quotient overflow boundary and the largest valid quotient. */
            check((uint64_t)rates[i] << 32, rates[i], 1360);
            check(((uint64_t)rates[i] << 32) - 1, rates[i], 1361);
        }
        for (unsigned i = 0; i < 10000; ++i) {
            uint64_t ticks = ((uint64_t)random_word() << 32) | random_word();
            check(ticks, random_word() | 1, (random_word() & 65535) + 1);
            check(ticks & UINT32_MAX, 709379, 1360);
        }
    }
    printf("PIT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
