#ifndef PCIINIT_PIT_H
#define PCIINIT_PIT_H
#include <stdint.h>
/* The 68020 divider accepts a 64-bit dividend when the quotient fits in
 * 32 bits. GCC's general 64-bit division otherwise uses a software loop.
 * Keep the full-range fallback for long uptimes and arbitrary clocks. */
static inline uint64_t pi_pit_divmod(uint64_t value, uint32_t divisor, uint32_t *remainder)
{
#ifdef __mc68000__
    uint32_t high = value >> 32, low = value;
    if (high < divisor) {
        __asm__("divu.l %2,%1:%0" : "+d"(low), "+d"(high) : "d"(divisor) : "cc");
        *remainder = high;
        return low;
    }
#endif
    *remainder = value % divisor;
    return value / divisor;
}
static inline uint64_t pi_pit_ticks(uint64_t clock, uint32_t hz)
{
    uint32_t fraction, unused;
    uint64_t seconds = pi_pit_divmod(clock, hz, &fraction);
    return seconds * 1193182 + pi_pit_divmod((uint64_t)fraction * 1193182, hz, &unused);
}
static inline uint32_t pi_pit_elapsed(uint64_t ticks, uint32_t reload)
{
    uint32_t remainder;
    pi_pit_divmod(ticks, reload, &remainder);
    return remainder;
}
/* The count a channel shows `elapsed` ticks into its period of `reload`
 * ticks. A square wave (mode 3) counts down by two and reloads at each
 * half period, the rate generator and the other modes count down by one. */
static inline uint32_t pi_pit_count(uint32_t reload, uint32_t elapsed, unsigned square)
{
    uint32_t half = (reload + 1) / 2;
    if (!square)
        return reload - elapsed;
    if (elapsed >= half)
        return (reload & ~1U) - 2 * (elapsed - half);
    if (elapsed && (reload & 1))
        return reload + 1 - 2 * elapsed;
    return reload - 2 * elapsed;
}
#endif
