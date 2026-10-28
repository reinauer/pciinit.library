#ifndef AMIGA_LIBRARIES_X86EMU_H
#define AMIGA_LIBRARIES_X86EMU_H
#include <stdint.h>

#ifdef __amigaos__
#define X86_CALLBACK __attribute__((stkparm))
#else
#define X86_CALLBACK
#endif

#define X86EMU_ABI_VERSION 1
#define X86EMU_VERSION     5 /* x86emu.library version to pass to OpenLibrary() */
typedef struct X86Context X86Context;

/* Stack C callbacks: declare implementations with X86_CALLBACK even when
 * internal functions use register arguments. No -mshort/-msmall-code.
 * D2-D7/A2-A6 are callee-saved. The library preserves A4 across calls;
 * clients requiring an A4 data base must supply a restore-a4 trampoline.
 * user is always passed explicitly; no upstream types cross this ABI. */
enum X86Access
{
    X86_READ,
    X86_WRITE,
    X86_FETCH,
    X86_IN,
    X86_OUT
};
enum X86Stop
{
    X86_STOP_HALT = 0,
    X86_STOP_LIMIT,
    X86_STOP_ACCESS,
    X86_STOP_FAULT,
    X86_STOP_CANCEL,
    X86_STOP_TIMEOUT,
    X86_STOP_BADARG,
    X86_STOP_BUSY
};
enum X86Segment
{
    X86_ES,
    X86_CS,
    X86_SS,
    X86_DS,
    X86_FS,
    X86_GS
};
struct X86Registers
{
    uint32_t size;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    uint16_t segment[6];
};
typedef uint32_t X86_CALLBACK (*X86AccessFn)(void *user, uint32_t address, uint32_t width, uint32_t operation,
                                             uint32_t *value);
/* 0: chain to IVT, 1: handled, other: stop as a fault. */
typedef uint32_t X86_CALLBACK (*X86InterruptFn)(void *user, uint32_t number, uint32_t fault,
                                                struct X86Registers *registers);
#define X86_MAX_CODE_REGIONS 8
#define X86_BULK_MAX         256
#define X86_SPAN_MAX         65536
/* Side-effect-free executable RAM/ROM. The descriptors are copied; data
 * must remain valid until context deletion. No concurrent writes during
 * X86Run are permitted. Guest writes and synchronous client callbacks may
 * modify the data; a backend must observe them before executing subsequent
 * code. Fetches in these ranges bypass access(), but still consume the
 * normal instruction-fetch access budget. */
struct X86CodeRegion
{
    uint32_t address, bytes;
    const uint8_t *data;
};
/* Bounded string transfers. Return the number of complete elements
 * processed, at most count. Zero declines without side effects. A partial
 * prefix must be equivalent to scalar access() calls in order (COPY reads
 * then writes each element). The engine supplies contiguous, non-wrapping
 * segment ranges of at most bulk_elements elements. The callback must
 * preserve memory/device semantics, including aliasing and latches, and
 * complete the longest prefix it can prove rather than declining a request
 * whose later elements leave an aperture, RAM range or address slice. MMIO
 * and port I/O must never be treated as ordinary RAM. The engine polls
 * cancellation and timeout once before each request and accounts every
 * element after it; after a decline it uses scalar accesses up to the next
 * 256-access boundary before offering another request. */
enum X86BulkOperation
{
    X86_BULK_FILL,
    X86_BULK_COPY
};
struct X86BulkRequest
{
    uint32_t size, operation, source, destination, width, count, value;
    int32_t stride;
};
typedef uint32_t X86_CALLBACK (*X86BulkFn)(void *user, const struct X86BulkRequest *request);
/* REP SCAS batching. Read up to count elements (at most X86_BULK_MAX) in
 * scalar order, stopping immediately after the first comparison for which
 * (value == expected) differs from repeat_equal. Return the processed
 * count and the last value read; zero declines with no side effects.
 * Preserve device read effects/latches, and never read beyond the
 * terminating element. The engine supplies bounded, contiguous segment
 * ranges and retains flags, counts, limits and polling. */
struct X86ScanRequest
{
    uint32_t size, address, width, count, expected, repeat_equal;
    int32_t stride;
};
typedef uint32_t X86_CALLBACK (*X86ScanFn)(void *user, const struct X86ScanRequest *request, uint32_t *last_value);
struct X86Config
{
    uint32_t size, version; /* sizeof(struct X86Config), X86EMU_ABI_VERSION */
    X86AccessFn access;
    X86InterruptFn interrupt; /* optional */
    void *user;
    uint32_t region_count; /* 0..X86_MAX_CODE_REGIONS */
    const struct X86CodeRegion *regions;
    X86BulkFn bulk; /* optional */
    X86ScanFn scan; /* optional */
    /* Largest bulk request, 1..X86_SPAN_MAX; zero means X86_BULK_MAX. */
    uint32_t bulk_elements;
    /* Bit i marks code region i as ordinary RAM which the engine reads and
     * writes directly for data accesses; access() does not see them. Other
     * regions are fetch-only. Bits beyond region_count are rejected. */
    uint32_t data_regions;
};
struct X86Limits
{
    uint32_t size, instructions, accesses, milliseconds;
    const volatile uint32_t *cancel;
};
struct X86Result
{
    uint32_t size, reason, instructions, accesses;
    uint32_t address, operation, interrupt;
};
#endif
