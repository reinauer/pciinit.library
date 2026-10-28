#ifndef BENCH_H
#define BENCH_H
#include <stdint.h>
extern unsigned char ram[65536];
extern unsigned io_count;
extern uint32_t io_sum;
struct result
{
    uint32_t ax, bx, cx, dx, si, di, bp, sp, ip, flags;
};
void engine_reset(void);
void engine_seed(const uint32_t registers[8], uint32_t flags);
int differential(void);
struct result engine_run(void);
unsigned engine_bytes(void);
uint32_t read_mem(unsigned addr, unsigned width);
void write_mem(unsigned addr, unsigned width, uint32_t value);
uint32_t read_port(unsigned port);
void write_port(unsigned port, uint32_t value);
#endif
