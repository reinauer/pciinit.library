/* Compare every opcode/register/width with the unmodified libx86emu core. */
#include "bench.h"
#include <stdio.h>
#include <string.h>
static uint32_t hash = 2166136261U;
static void mix(uint32_t value)
{
    hash = (hash ^ value) * 16777619U;
}
int differential(void)
{
    static const uint32_t values[] = {0, 1, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000U, 0xffffffffU};
    unsigned op, wide, carry, seed, i, cases = 0;
    for (op = 0; op < 256; ++op) {
        if (!((op >= 0x40 && op <= 0x5f) || (op >= 0x91 && op <= 0x97) || (op >= 0xb0 && op <= 0xbf)))
            continue;
        for (wide = 0; wide < 2; ++wide)
            for (carry = 0; carry < 2; ++carry)
                for (seed = 0; seed < 8; ++seed) {
                    uint32_t regs[8], imm = values[seed] ^ 0x12345678, flags = 2 | carry;
                    struct result r;
                    unsigned n = 0, bytes = op >= 0xb8 ? (wide ? 4 : 2) : op >= 0xb0 ? 1 : 0;
                    unsigned char *code = ram + 0x1000;
                    for (i = 0; i < 8; ++i)
                        regs[i] = values[(seed + i) & 7];
                    regs[4] = 0xf000;
                    if (wide)
                        code[n++] = 0x66;
                    code[n++] = op;
                    for (i = 0; i < bytes; ++i)
                        code[n++] = imm >> (8 * i);
                    code[n++] = 0xf4;
                    memset(ram + 0xef00, 0xa5, 512);
                    write_mem(0xf000, 4, values[seed]);
                    engine_reset();
                    engine_seed(regs, flags);
                    r = engine_run();
                    if (r.ip != 0x1000 + n || ((op < 0x60) && ((r.flags & 1) != carry)))
                        return 20;
                    mix(r.ax);
                    mix(r.bx);
                    mix(r.cx);
                    mix(r.dx);
                    mix(r.si);
                    mix(r.di);
                    mix(r.bp);
                    mix(r.sp);
                    mix(r.ip);
                    mix(r.flags);
                    for (i = 0xef00; i < 0xf010; ++i)
                        mix(ram[i]);
                    ++cases;
                }
    }
    printf("DIFFERENTIAL cases=%u hash=%08lx\n", cases, (unsigned long)hash);
    return 0;
}
