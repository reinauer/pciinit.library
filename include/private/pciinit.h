#ifndef PCIINIT_PRIVATE_H
#define PCIINIT_PRIVATE_H
#include "library.h"
#include "rom.h"
#include "runtime.h"
#include "openpci_calls.h"
#include "x86emu_calls.h"
#include "bus.h"
#include <utility/hooks.h>
#define PI_RAM_BYTES 0x100000UL
#define PI_ROM_BASE  0xc0000UL
#define PI_RETURN    0x7000UL
struct PCIInitImage
{
    struct PIBase *owner;
    struct PCIInitROMInfo info;
    uint8_t *data;
    uint32_t crc; /* CRC-32 of the initialization image, for known-image quirks */
};
/* One assigned memory BAR: PCI address, CPU address, size and BAR number. */
struct PIBar
{
    uint32_t bus, cpu, size, index;
};
#define PI_LEGACY_BASE  0xa0000UL
#define PI_LEGACY_BYTES 0x20000UL
struct PCIInitContext
{
    struct PIBase *owner;
    struct Library *pci;
    struct pci_dev *board;
    X86Context *cpu;
    uint8_t *ram;
    struct X86Limits limits;
    struct PCIInitContextInfo info;
    uint16_t vendor, device;
    uint32_t hardware, io, bdf, command, cf8;
    APTR legacy;           /* CPU address of PCI A0000-BFFFF, or NULL */
    uint8_t legacy_missed; /* the ROM used the window while it was missing */
    uint8_t bars_visible;  /* BAR addresses stay valid while the window is held */
    uint8_t bar_count;
    struct PIBar bars[6];
    struct MsgPort *timer_port;
    struct timerequest *timer;
    uint64_t pit_started; /* PIT channel 2 */
    uint32_t pit_reload;
    uint16_t pit_latch;
    uint8_t pit_phase, pit_low, pit_read;
    uint8_t port61;
    uint64_t pit0_started; /* PIT channel 0; zero fields mean the system BIOS default */
    uint32_t pit0_reload;
    uint16_t pit0_latch;
    uint8_t pit0_phase, pit0_low, pit0_read, pit0_control;
    uint8_t dma_page[16]; /* DMA page registers 80h-8Fh: read/write latches, 80h is the POST code */
    struct PCIInitDiagnostics *diagnostics;
    uint32_t vbe_version, vbe_memory, mode_flags, mode_valid, map_command;
    struct PCIInitVBEModeInfo mode;
    uint32_t window_granularity, window_size, bank; /* VBE window geometry of the set mode, KiB */
    uint16_t window_segment[2];
    uint8_t window_attributes[2];
    APTR framebuffer;
    uint8_t quirks;
    uint8_t stop_bus_error;
    uint8_t bus_routines; /* card accesses use the trap-safe bus.S routines (68040/060) */
    uint8_t decode;       /* PCI_COMMAND I/O and memory decode as the ROM last set them */
    uint8_t detour;       /* card accesses take detour_card(): a hook is set or a decode is off */
    uint8_t hook_changed; /* the hook changed or completed the current access */
    uint8_t range_count;  /* 0: the hook sees every card access */
    struct Hook *hook;
    struct PCIInitAccessRange ranges[PI_MAX_ACCESS_RANGES];
    struct PCIInitServiceLog *services;
    struct PIBusTrap trap; /* installed between pi_hardware_start and pi_hardware_finish */
    uint32_t faults_seen;  /* trap.faults already turned into a stop */
};
struct PIOptions
{
    uint32_t *error;
    uint32_t offline, hardware, quirks, stop_bus_error, range_count;
    struct PCIInitDiagnostics *diagnostics;
    const struct X86Limits *limits;
    struct Hook *hook;
    const struct PCIInitAccessRange *ranges;
    struct PCIInitServiceLog *services;
};
uint32_t pi_options(const struct TagItem *, struct PIOptions *);
PCIInitImage *pi_load_file(struct PIBase *, const char *, const struct TagItem *);
PCIInitImage *pi_load_card(struct PIBase *, struct pci_dev *, const struct TagItem *);
uint32_t pi_free_image(struct PIBase *, PCIInitImage *);
PCIInitContext *pi_create(struct PIBase *, struct pci_dev *, const struct TagItem *);
uint32_t pi_delete(struct PIBase *, PCIInitContext *);
uint32_t pi_run(struct PIBase *, PCIInitContext *, const PCIInitImage *);
void pi_trace(PCIInitContext *, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t pi_hardware_scan(PCIInitContext *, const struct X86ScanRequest *, uint32_t *);
uint32_t pi_hardware_bulk(PCIInitContext *, const struct X86BulkRequest *, const uint8_t *);
uint32_t pi_hardware_start(PCIInitContext *);
void pi_hardware_finish(PCIInitContext *);
uint32_t pi_hardware_access(PCIInitContext *, uint32_t, uint32_t, uint32_t, uint32_t *);
uint32_t pi_bios(PCIInitContext *, uint32_t, struct X86Registers *);
uint32_t pi_int10(struct PIBase *, PCIInitContext *, struct X86Registers *);
uint32_t pi_vbe_info(struct PIBase *, PCIInitContext *, struct PCIInitVBEInfo *);
uint32_t pi_vbe_mode_info(struct PIBase *, PCIInitContext *, uint32_t, struct PCIInitVBEModeInfo *);
uint32_t pi_vbe_set(struct PIBase *, PCIInitContext *, uint32_t, uint32_t);
uint32_t pi_vbe_current(struct PIBase *, PCIInitContext *, uint32_t *);
uint32_t pi_fb_map(struct PIBase *, PCIInitContext *, struct PCIInitFramebuffer *);
uint32_t pi_fb_unmap(struct PIBase *, PCIInitContext *);
uint32_t pi_fb_transfer(struct PIBase *, PCIInitContext *, uint32_t, void *, uint32_t, unsigned);
int pi_board_exists(struct Library *, struct pci_dev *);
#endif
