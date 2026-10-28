/* Identical bounded programs and checked results for both engines. */
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
unsigned char ram[65536];
unsigned io_count;
uint32_t io_sum;
uint32_t read_mem(unsigned a, unsigned width)
{
    uint32_t value = 0;
    unsigned i;
    if (a > sizeof(ram) || width > sizeof(ram) - a || width > 4)
        exit(24);
    for (i = 0; i < width; ++i)
        value |= (uint32_t)ram[a + i] << (8 * i);
    return value;
}
void write_mem(unsigned a, unsigned width, uint32_t value)
{
    unsigned i;
    if (a > sizeof(ram) || width > sizeof(ram) - a || width > 4)
        exit(25);
    for (i = 0; i < width; ++i)
        ram[a + i] = value >> (8 * i);
}
uint32_t read_port(unsigned port)
{
    return (port ^ 0xa5) & 255;
}
void write_port(unsigned port, uint32_t value)
{
    ++io_count;
    io_sum += port + (value & 255);
}
static const unsigned char alu16[] = {0xb9, 0,    4, 0xb8, 1,    0,    0xbb, 3,    0,    0xba, 7, 0,    0x01, 0xd8,
                                      0x83, 0xd2, 0, 0x35, 0xaa, 0x55, 0xd1, 0xc0, 0x83, 0xc3, 7, 0xe2, 0xf1, 0xf4};
static const unsigned char alu32[] = {0xb9, 0,    4,    0x66, 0xb8, 1,    0,    0,    0,    0x66, 0xbb,
                                      3,    0,    0,    0,    0x66, 0xba, 7,    0,    0,    0,    0x66,
                                      0x01, 0xd8, 0x66, 0x83, 0xd2, 0,    0x66, 0x35, 0xaa, 0x55, 0x33,
                                      0xcc, 0x66, 0xd1, 0xc0, 0x66, 0x83, 0xc3, 7,    0xe2, 0xea, 0xf4};
static const unsigned char mem[]   = {0xb9, 0,    4, 0xbe, 0,    0x40, 0xbf, 0,    0x60, 0x8b, 4,    0x05, 0xaa,
                                      0x55, 0x89, 5, 0x83, 0xc6, 2,    0x83, 0xc7, 2,    0xe2, 0xf1, 0xf4};
static const unsigned char rep[]   = {0xb9, 0, 0x10, 0xbe, 0, 0x40, 0xbf, 0, 0x60, 0xf3, 0xa4, 0xf4};
static const unsigned char io[]    = {0xb9, 0, 4, 0xba, 0, 1, 0xec, 0x34, 0x55, 0xee, 0x42, 0xe2, 0xf9, 0xf4};
static const unsigned char registers16[] = {0xb9, 0x00, 0x04, 0xbb, 0x03, 0x00, 0xb8, 0x34, 0x12,
                                            0xb4, 0xaa, 0x40, 0x4b, 0x50, 0x5a, 0x92, 0xbe, 0x78,
                                            0x56, 0x46, 0x4e, 0x56, 0x5f, 0x93, 0xe2, 0xec, 0xf4};
static const unsigned char registers32[] = {0xb9, 0x00, 0x04, 0x66, 0xbb, 0x03, 0x00, 0x00, 0x00, 0x66, 0xb8, 0x78,
                                            0x56, 0x34, 0x12, 0xb4, 0xaa, 0x66, 0x40, 0x66, 0x4b, 0x66, 0x50, 0x66,
                                            0x5a, 0x66, 0x92, 0x66, 0xbe, 0xbc, 0x9a, 0x78, 0x56, 0x66, 0x46, 0x66,
                                            0x4e, 0x66, 0x56, 0x66, 0x5f, 0x66, 0x93, 0xe2, 0xdc, 0xf4};
struct workload
{
    const char *name;
    const unsigned char *code;
    unsigned bytes;
};
static const struct workload workloads[] = {{"alu16", alu16, sizeof(alu16)},
                                            {"alu32", alu32, sizeof(alu32)},
                                            {"memory", mem, sizeof(mem)},
                                            {"rep", rep, sizeof(rep)},
                                            {"io", io, sizeof(io)},
                                            {"registers16", registers16, sizeof(registers16)},
                                            {"registers32", registers32, sizeof(registers32)}};
static int verify(unsigned n, struct result r)
{
    uint32_t a = 1, b = 3, d = 7, mask = n == 0 ? 65535 : 0xffffffffU;
    unsigned i;
    if (r.ip != 0x1000 + workloads[n].bytes || r.cx || r.sp != 0xf000)
        return 0;
    if (n < 2) {
        for (i = 0; i < 1024; ++i) {
            uint32_t next = (a + b) & mask;
            d             = (d + (next < a)) & mask;
            a             = next ^ (n == 0 ? 0x55aa : 0xcc3355aaU);
            a             = ((a << 1) | (a >> (n == 0 ? 15 : 31))) & mask;
            b             = (b + 7) & mask;
        }
        return r.ax == a && r.bx == b && r.dx == d;
    }
    if (n == 2) {
        for (i = 0; i < 1024; ++i)
            if (read_mem(0x6000 + 2 * i, 2) != ((read_mem(0x4000 + 2 * i, 2) + 0x55aa) & 65535))
                return 0;
        return r.si == 0x4800 && r.di == 0x6800;
    }
    if (n == 3)
        return r.si == 0x5000 && r.di == 0x7000 && !memcmp(ram + 0x4000, ram + 0x6000, 4096);
    if (n >= 5) {
        a = n == 5 ? 0xaa35 : 0x1234aa79;
        b = n == 5 ? 0x5678 : 0x56789abc;
        return r.ax == a - 1 && r.bx == a && r.dx == a && r.si == b && r.di == b;
    }
    a = 0;
    for (i = 0; i < 1024; ++i)
        a += 0x100 + i + ((read_port(0x100 + i) ^ 0x55) & 255);
    return io_count == 1024 && io_sum == a && r.dx == 0x500;
}
static double seconds(void)
{
#ifdef __amigaos__
    return 0; /* Target comparison uses Vamos CPU cycles, not host time. */
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
int main(int argc, char **argv)
{
    unsigned n, repeats, i, j;
    double start, elapsed;
    struct result r = {0};
    if (argc == 2 && !strcmp(argv[1], "check"))
        return differential();
    if (argc != 3)
        return 20;
    n       = strtoul(argv[1], NULL, 10);
    repeats = strtoul(argv[2], NULL, 10);
    if (n >= sizeof(workloads) / sizeof(workloads[0]) || !repeats)
        return 20;
    for (i = 0; i < sizeof(ram); ++i)
        ram[i] = (i * 17 + 3) & 255;
    memcpy(ram + 0x1000, workloads[n].code, workloads[n].bytes);
    start = seconds();
    for (j = 0; j < repeats; ++j) {
        engine_reset();
        io_count = 0;
        io_sum   = 0;
        r        = engine_run();
        if (!verify(n, r)) {
            printf("FAIL %s ax=%lx bx=%lx cx=%lx dx=%lx si=%lx di=%lx ip=%lx\n", workloads[n].name, (unsigned long)r.ax,
                   (unsigned long)r.bx, (unsigned long)r.cx, (unsigned long)r.dx, (unsigned long)r.si,
                   (unsigned long)r.di, (unsigned long)r.ip);
            return 20;
        }
    }
    elapsed = seconds() - start;
    printf("BENCH %s repeats=%u seconds=%.9f state=%u verified=1\n", workloads[n].name, repeats, elapsed,
           engine_bytes());
    return 0;
}
