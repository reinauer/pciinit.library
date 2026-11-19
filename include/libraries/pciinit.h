#ifndef AMIGA_LIBRARIES_PCIINIT_H
#define AMIGA_LIBRARIES_PCIINIT_H
#include <stdint.h>
#include <libraries/x86emu.h>

struct pci_dev;
typedef struct PCIInitImage PCIInitImage;
typedef struct PCIInitContext PCIInitContext;
enum PCIInitStatus
{
    PI_OK,
    PI_BAD_ARGUMENT,
    PI_NO_MEMORY,
    PI_IO_ERROR,
    PI_BAD_ROM,
    PI_UNSUPPORTED_ROM,
    PI_NO_OPENPCI,
    PI_CARD_BUSY,
    PI_NO_ROM,
    PI_WRONG_CARD,
    PI_UNSUPPORTED_MAPPING,
    PI_BAD_STATE,
    PI_EXECUTION_FAILED,
    PI_VBE_FAILED,
    PI_UNSUPPORTED_MODE
};
enum PCIInitState
{
    PI_CREATED,
    PI_EXECUTING,
    PI_INITIALIZED,
    PI_FAILED
};
/* Tags accept ordinary TAG_DONE/IGNORE/SKIP/MORE control items. */
#define PII_Error          (0x80005001UL) /* uint32_t *: optional result */
#define PII_Offline        (0x80005002UL) /* nonzero: synthetic RAM-only context */
#define PII_Limits         (0x80005003UL) /* const struct X86Limits *: copied */
#define PII_Diagnostics    (0x80005004UL) /* struct PCIInitDiagnostics * */
#define PII_Hardware       (0x80005005UL) /* nonzero: run the ROM on the owned card */
#define PII_Quirks         (0x80005006UL) /* zero: run known ROM images unpatched; default on */
#define PII_AccessHook     (0x80005007UL) /* struct Hook *: sees the card's accesses, see PCIInitAccessMsg */
#define PII_AccessRanges   (0x80005008UL) /* const struct PCIInitAccessRange *: hook only these; copied */
#define PII_ServiceLog     (0x80005009UL) /* struct PCIInitServiceLog *: missing BIOS services fail and are logged */
#define PII_StopOnBusError (0x8000500AUL) /* nonzero: a 68k bus error on a card access stops the run */

/* Card address spaces as an access hook sees them. Offsets are ports for
 * I/O, from A0000 for the legacy window, from the BAR's start for memory
 * BARs and the register number for configuration space. */
enum PCIInitSpace
{
    PI_SPACE_IO,
    PI_SPACE_LEGACY,
    PI_SPACE_BAR,
    PI_SPACE_CONFIG
};
#define PI_SPACE_END         0xffffffffUL /* ends a PCIInitAccessRange list */
#define PI_MAX_ACCESS_RANGES 16
struct PCIInitAccessRange
{
    uint32_t space, bar, first, last; /* bar: BAR number 0-5, PI_SPACE_BAR only; first/last inclusive */
};
#define PI_PHASE_BEFORE 0 /* every access, before it reaches the card */
#define PI_PHASE_AFTER  1 /* passed reads, with the value the card returned */
/* The hook is called as h_Entry(hook, context, message) in A0, A2, A1 and
 * returns PI_HOOK_*. value holds the guest's little-endian number; the
 * hook may change it before a write or after a read. */
struct PCIInitAccessMsg
{
    uint32_t size, space, phase, write, bar, offset, width, value;
};
#define PI_HOOK_PASS  0 /* perform the access with value */
#define PI_HOOK_DONE  1 /* BEFORE: the hook completed it, a read returns value and a write is dropped */
#define PI_HOOK_FAULT 2 /* stop the run as an access fault */
/* Missing BIOS services as vector<<16 | AX, each listed once. */
struct PCIInitServiceLog
{
    uint32_t size, capacity, count;
    uint32_t *entries;
};
struct PCIInitTraceEntry
{
    uint32_t operation, address, width, value;
};
struct PCIInitDiagnostics
{
    uint32_t size, capacity, count;
    struct PCIInitTraceEntry *entries;
    struct X86Registers registers;
    uint32_t io_accesses, vga_accesses, bar_accesses, int10_vector;
};
#define PI_TRACE_INTERRUPT 5
#define PI_TRACE_HOOKED    0x100UL /* or'ed into operation when an access hook changed or completed the access */
#define PCIINIT_MAX_ROM    (1024UL * 1024UL)
struct PCIInitROMInfo
{
    uint32_t size, source_bytes, image_offset, image_bytes, init_bytes;
    uint16_t vendor, device;
    uint32_t image_count;
    const uint8_t *source; /* the loaded bytes, source_bytes long, read-only while the image exists */
};
struct PCIInitContextInfo
{
    uint32_t size, state, status, offline;
    struct X86Result execution;
    uint32_t warnings;        /* PI_WARN_* bits from the last POST and later BIOS calls */
    uint32_t missing_service; /* vector<<16 | AX of the first BIOS call nobody answered */
    /* Since 2.4; GetPCIContextInfo() also accepts the size without these. */
    uint32_t bus_errors;        /* 68k bus errors on card accesses since POST started */
    uint32_t bus_error_address; /* Amiga address of the first one */
};
/* The ROM accessed legacy VGA memory (A0000-BFFFF) and the PCI bus board
 * cannot forward that range: reads returned zero and writes were dropped. */
#define PI_WARN_NO_LEGACY_WINDOW 1UL
/* The ROM called a BIOS service that neither it nor the library provides;
 * the run stopped there as a fault and missing_service names the call. */
#define PI_WARN_MISSING_SERVICE 2UL
/* A card access ended in a 68k bus error. Reads returned all-ones and
 * writes were dropped, as for an aborted PCI cycle, and the ROM went on
 * unless PII_StopOnBusError was set. */
#define PI_WARN_BUS_ERROR       4UL
#define PCIINIT_LIBRARY_VERSION 2 /* pciinit.library version to pass to OpenLibrary() */
#define PI_VBE_MAX_MODES        256
#define PI_VBE_LINEAR           0x4000UL
#define PI_VBE_KEEP_MEMORY      0x8000UL
#define PI_FB_LITTLE_ENDIAN     1UL
#define PI_FB_INDEXED           2UL
struct PCIInitVBEInfo
{
    uint32_t size, version, capabilities, memory_bytes, mode_count;
    char oem[128];
    uint16_t modes[PI_VBE_MAX_MODES];
};
struct PCIInitVBEModeInfo
{
    uint32_t size, mode, attributes, width, height, pitch, bits_per_pixel;
    uint32_t memory_model, planes, framebuffer_pci, image_pages;
    uint32_t red_size, red_position, green_size, green_position;
    uint32_t blue_size, blue_position, reserved_size, reserved_position;
};
struct PCIInitFramebuffer
{
    uint32_t size;
    volatile uint8_t *address;
    uint32_t bytes, flags;
    struct PCIInitVBEModeInfo mode;
};
#endif
