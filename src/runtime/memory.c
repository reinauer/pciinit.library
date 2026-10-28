#include "private/runtime.h"
#include <string.h>
#ifdef __amigaos__
#include <exec/memory.h>
#include <proto/exec.h>
void *pi_alloc(size_t bytes)
{
    struct ExecBase *SysBase = pi_sysbase();
    return bytes ? AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR) : NULL;
}
void pi_free(void *memory)
{
    struct ExecBase *SysBase = pi_sysbase();
    if (memory)
        FreeVec(memory);
}
void *memcpy(void *dest, const void *src, size_t length)
{
    unsigned char *d       = dest;
    const unsigned char *s = src;
    /* 68020+ permits unaligned word/long data accesses. Only ordinary
     * memory reaches these C routines; device transfers use volatile I/O. */
    while (length >= 16) {
        ((uint32_t *)d)[0] = ((const uint32_t *)s)[0];
        ((uint32_t *)d)[1] = ((const uint32_t *)s)[1];
        ((uint32_t *)d)[2] = ((const uint32_t *)s)[2];
        ((uint32_t *)d)[3] = ((const uint32_t *)s)[3];
        d += 16;
        s += 16;
        length -= 16;
    }
    while (length >= 4) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
        length -= 4;
    }
    while (length--)
        *d++ = *s++;
    return dest;
}
void *memset(void *dest, int value, size_t length)
{
    unsigned char *d = dest;
    uint32_t word    = (uint8_t)value;
    word |= word << 8;
    word |= word << 16;
#ifdef __mc68000__
    /* Large fills: eight longwords per MOVEM from a longword boundary.
     * Guest RAM resets and context clears are megabyte-sized. */
    if (length >= 64) {
        uint32_t blocks;
        while ((uintptr_t)d & 3) {
            *d++ = value;
            --length;
        }
        blocks = length / 32;
        __asm__ volatile(
            "move.l %2,%%d0\n\tmove.l %%d0,%%d1\n\t"
            "move.l %%d0,%%d2\n\tmove.l %%d0,%%d3\n\t"
            "move.l %%d0,%%d4\n\tmove.l %%d0,%%d5\n\t"
            "move.l %%d0,%%d6\n\tmove.l %%d0,%%d7\n"
            "1:\tmovem.l %%d0-%%d7,(%0)\n\tlea 32(%0),%0\n\t"
            "subq.l #1,%1\n\tcmpa.w #0,%1\n\tbne 1b"
            : "+a"(d), "+a"(blocks)
            : "g"(word)
            : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "cc", "memory");
        length &= 31;
    }
#endif
    while (length >= 16) {
        ((uint32_t *)d)[0] = word;
        ((uint32_t *)d)[1] = word;
        ((uint32_t *)d)[2] = word;
        ((uint32_t *)d)[3] = word;
        d += 16;
        length -= 16;
    }
    while (length >= 4) {
        *(uint32_t *)d = word;
        d += 4;
        length -= 4;
    }
    while (length--)
        *d++ = value;
    return dest;
}
int memcmp(const void *left, const void *right, size_t length)
{
    const unsigned char *a = left, *b = right;
    while (length >= 4 && *(const uint32_t *)a == *(const uint32_t *)b) {
        a += 4;
        b += 4;
        length -= 4;
    }
    while (length--) {
        if (*a != *b)
            return *a - *b;
        ++a;
        ++b;
    }
    return 0;
}
#else
#include <stdlib.h>
void *pi_alloc(size_t bytes)
{
    return calloc(1, bytes);
}
void pi_free(void *memory)
{
    free(memory);
}
#endif
