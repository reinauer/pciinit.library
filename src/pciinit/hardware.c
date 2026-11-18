#include "private/pciinit.h"
#include "private/record.h"
#include "private/pit.h"
#include <devices/timer.h>
#include <proto/timer.h>

/* Card-independent transport for a ROM running on its own card: legacy
 * VGA memory through the bridge's ObtainPCIRegion window, I/O ports
 * through the bridge's I/O aperture, the card's assigned memory BARs where
 * the bridge keeps them reachable, plus the PC timer, PCI BIOS and
 * configuration services a ROM expects. Nothing here knows a chip. */

static uint64_t pit_ticks(PCIInitContext *c)
{
    struct Library *TimerBase = (struct Library *)c->timer->tr_node.io_Device;
    struct EClockVal clock;
    uint32_t hz    = ReadEClock(&clock);
    uint64_t ticks = ((uint64_t)clock.ev_hi << 32) | clock.ev_lo;
    return pi_pit_ticks(ticks, hz);
}
/* Channel 0 as the system BIOS leaves it: free running since power-up in
 * mode 3 with a reload of 65536, until the ROM programs it. */
static uint32_t pit0_count(PCIInitContext *c)
{
    uint32_t reload = c->pit0_reload ? c->pit0_reload : 65536;
    unsigned mode   = (c->pit0_control >> 1) & 7;
    unsigned square = !c->pit0_control || mode == 3 || mode == 7;
    return pi_pit_count(reload, pi_pit_elapsed(pit_ticks(c) - c->pit0_started, reload), square) & 0xffff;
}

static uint32_t swap(uint32_t v, unsigned width)
{
    if (width == 2)
        return ((v & 255) << 8) | ((v >> 8) & 255);
    if (width == 4)
        return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
    return v;
}
/* PCI memory keeps byte addresses; a little-endian guest value is stored
 * with its low byte first, which is a swapped 68k word or longword. */
static uint32_t transaction(uint32_t address, unsigned width, unsigned write, uint32_t value)
{
    if (write) {
        value = swap(value, width);
        if (width == 1)
            *(volatile uint8_t *)address = value;
        else if (width == 2)
            *(volatile uint16_t *)address = value;
        else
            *(volatile uint32_t *)address = value;
        return 0;
    }
    if (width == 1)
        value = *(volatile uint8_t *)address;
    else if (width == 2)
        value = *(volatile uint16_t *)address;
    else
        value = *(volatile uint32_t *)address;
    return swap(value, width);
}
void pi_trace(PCIInitContext *c, uint32_t op, uint32_t addr, uint32_t width, uint32_t value)
{
    struct PCIInitDiagnostics *d = c->diagnostics;
    if (d && d->capacity) {
        struct PCIInitTraceEntry *e = &d->entries[d->count % d->capacity];
        *e                          = (struct PCIInitTraceEntry){op, addr, width, value};
        ++d->count;
    }
}
static int legacy_range(uint32_t address, uint32_t width)
{
    return address >= PI_LEGACY_BASE && address - PI_LEGACY_BASE < PI_LEGACY_BYTES &&
           width <= PI_LEGACY_BYTES - (address - PI_LEGACY_BASE);
}
/* Ports a PC chipset answers itself (DMA, interrupt controllers, timer,
 * keyboard, CMOS, DMA pages, coprocessor): no card ever sees them. */
static int chipset_port(uint32_t a)
{
    return a < 0x22 || (a >= 0x40 && a < 0x44) || (a >= 0x60 && a < 0x65) || a == 0x70 || a == 0x71 ||
           (a >= 0x80 && a < 0x90) || a == 0xa0 || a == 0xa1 || (a >= 0xc0 && a < 0xe0) || (a >= 0xf0 && a < 0x100);
}
static const struct PIBar *find_bar(const PCIInitContext *c, uint32_t address, uint32_t width)
{
    unsigned i;
    for (i = 0; i < c->bar_count; ++i) {
        const struct PIBar *b = &c->bars[i];
        if (address >= b->bus && address - b->bus < b->size && width <= b->size - (address - b->bus))
            return b;
    }
    return NULL;
}
/* The assigned memory BARs, from public attributes only. */
static void read_bars(PCIInitContext *c)
{
    unsigned i;
    c->bar_count = 0;
    for (i = 0; i < 6; ++i) {
        uint32_t bus  = op_read(c->pci, c->board, PCI_BASE_ADDRESS_0 + 4 * i, 4);
        uint32_t cpu  = op_attr(c->pci, c->board, PRM_MemoryAddr0 + i);
        uint32_t size = op_attr(c->pci, c->board, PRM_MemorySize0 + i);
        if ((bus & 1) || !cpu || cpu == 0xffffffffU || !size)
            continue;
        bus &= ~15U;
        if (!bus || size > 0xffffffffU - bus || size > 0xffffffffU - cpu)
            continue;
        c->bars[c->bar_count++] = (struct PIBar){bus, cpu, size};
    }
}
uint32_t pi_hardware_start(PCIInitContext *c)
{
    struct ExecBase *SysBase = c->owner->sys;
    struct pci_dev *other    = NULL;
    if (c->owner->mapping_owner)
        return PI_BAD_STATE;
    if (!c->hardware)
        return PI_UNSUPPORTED_MAPPING;
    /* Classic VGA ports are shared. Do not initialize through another
     * display controller that is already decoding I/O on this bridge. */
    while ((other = op_find(c->pci, other))) {
        uint32_t classcode = op_read(c->pci, other, 8, 4) >> 8;
        if (other != c->board && ((classcode >> 16) == 3 || classcode == 0x000100) &&
            (op_read(c->pci, other, PCI_COMMAND, 2) & 1))
            return PI_UNSUPPORTED_MAPPING;
    }
    c->io = op_attr(c->pci, c->board, PRM_LegacyIOSpace);
    if (!c->io || c->io > 0xffff0000U)
        return PI_UNSUPPORTED_MAPPING;
    read_bars(c);
    c->bdf        = (op_attr(c->pci, c->board, PRM_BusNumber) << 8) | (op_attr(c->pci, c->board, PRM_SlotNumber) << 3) |
                    op_attr(c->pci, c->board, PRM_FunctionNumber);
    c->command    = op_read(c->pci, c->board, PCI_COMMAND, 2);
    c->timer_port = CreateMsgPort();
    if (c->timer_port)
        c->timer = (struct timerequest *)CreateIORequest(c->timer_port, sizeof(*c->timer));
    if (!c->timer || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)c->timer, 0)) {
        if (c->timer)
            DeleteIORequest((struct IORequest *)c->timer);
        if (c->timer_port)
            DeleteMsgPort(c->timer_port);
        c->timer      = NULL;
        c->timer_port = NULL;
        return PI_NO_MEMORY;
    }
    /* Ownership excludes another driver. Keep DMA disabled while the ROM
     * runs, including when firmware left bus mastering enabled at boot. */
    op_write(c->pci, c->board, PCI_COMMAND, (c->command | 3) & ~PCI_COMMAND_MASTER, 2);
    if (op_read(c->pci, c->board, PCI_COMMAND, 2) != ((c->command | 3) & ~PCI_COMMAND_MASTER)) {
        pi_hardware_finish(c);
        return PI_IO_ERROR;
    }
    /* The legacy VGA window is held for the whole run. A bridge that
     * cannot reach PCI A0000 returns NULL; the ROM then runs against an
     * empty window (reads zero, writes dropped) and the run is flagged
     * with PI_WARN_NO_LEGACY_WINDOW. */
    c->legacy_missed = 0;
    c->legacy        = op_region(c->pci, c->board, PI_LEGACY_BASE, PI_LEGACY_BYTES);
    /* A held region may hide every other PCI window. BAR accesses are
     * forwarded only where a region is the permanent address itself. */
    c->bars_visible = 1;
    if (c->legacy && c->bar_count) {
        uint32_t bytes = c->bars[0].size < 4096 ? c->bars[0].size : 4096;
        APTR probe;
        op_unregion(c->pci, c->board, c->legacy);
        probe           = op_region(c->pci, c->board, c->bars[0].bus, bytes);
        c->bars_visible = probe && (uint32_t)probe == c->bars[0].cpu;
        if (probe)
            op_unregion(c->pci, c->board, probe);
        c->legacy = op_region(c->pci, c->board, PI_LEGACY_BASE, PI_LEGACY_BYTES);
    }
    return PI_OK;
}
void pi_hardware_finish(PCIInitContext *c)
{
    struct ExecBase *SysBase = c->owner->sys;
    if (c->legacy)
        op_unregion(c->pci, c->board, c->legacy);
    if (c->legacy_missed)
        c->info.warnings |= PI_WARN_NO_LEGACY_WINDOW;
    c->legacy = NULL;
    op_write(c->pci, c->board, PCI_COMMAND, c->command, 2);
    CloseDevice((struct IORequest *)c->timer);
    DeleteIORequest((struct IORequest *)c->timer);
    DeleteMsgPort(c->timer_port);
    c->timer      = NULL;
    c->timer_port = NULL;
}
/* Reads enumerate only the owned card. No probes or relocation reach hardware. */
static uint32_t config(PCIInitContext *c, uint32_t bdf, uint32_t reg, unsigned width, unsigned write, uint32_t *value)
{
    if (reg >= 256 || width > 256 - reg || (reg & (width - 1)))
        return 1;
    if (bdf != c->bdf) {
        if (write)
            return 1;
        *value = 0xffffffffU;
        return 0;
    }
    if (write) {
        /* Permit no-op writes and command I/O/memory enables only. */
        if (*value == op_read(c->pci, c->board, reg, width))
            return 0;
        if (reg == PCI_COMMAND && width <= 2 && !(*value & ~3U)) {
            op_write(c->pci, c->board, reg, *value, width);
            return 0;
        }
        return 1;
    }
    *value = op_read(c->pci, c->board, reg, width);
    return 0;
}
#ifdef PCIINIT_RECORD
#define pi_hardware_access hardware_recorded_body
static
#endif
    uint32_t pi_hardware_access(PCIInitContext *c, uint32_t address, uint32_t width, uint32_t op, uint32_t *value)
{
    unsigned write  = op == X86_WRITE || op == X86_OUT;
    uint32_t result = 1;
    if (op == X86_IN || op == X86_OUT) {
        if (c->diagnostics)
            ++c->diagnostics->io_accesses;
        if (address >= 0x3b0 && address + width <= 0x3e0) {
            /* VGA indexed ports define adjacent byte operations for OUTW. */
            unsigned i;
            uint32_t v = 0;
            for (i = 0; i < width; ++i) {
                if (write)
                    *(volatile uint8_t *)(c->io + address + i) = *value >> (8 * i);
                else
                    v |= (uint32_t)*(volatile uint8_t *)(c->io + address + i) << (8 * i);
            }
            if (!write)
                *value = v;
            result = 0;
        } else if (address == 0xcf8 && width == 4) {
            if (write)
                c->cf8 = *value;
            else
                *value = c->cf8;
            result = 0;
        } else if (address >= 0xcfc && address + width <= 0xd00 && (c->cf8 & 0x80000000U)) {
            result = config(c, (c->cf8 >> 8) & 0xffff, (c->cf8 & 0xfc) + (address - 0xcfc), width, write, value);
        } else if (address == 0x80) {
            if (!write)
                *value = 255;
            result = 0;
        } else if (address == 0x43 && width == 1 && write && *value == 0xb6) {
            /* Channel 2, low/high reload, binary square-wave mode. */
            c->pit_phase  = 1;
            c->pit_reload = 0;
            result        = 0;
        } else if (address == 0x43 && width == 1 && write && *value == 0x80 && c->pit_reload) {
            if (!c->pit_read) {
                uint32_t elapsed = (c->port61 & 1) ? pi_pit_elapsed(pit_ticks(c) - c->pit_started, c->pit_reload) : 0;
                c->pit_latch     = pi_pit_count(c->pit_reload, elapsed, 1);
                c->pit_read      = 1;
            }
            result = 0;
        } else if (address == 0x43 && width == 1 && write && (*value & 0xc0) == 0) {
            /* Channel 0, the system tick: a latch command freezes the count
             * for the next two reads of port 40h, a control word with an
             * access mode starts a new reload sequence there. */
            unsigned access = (*value >> 4) & 3;
            if (access == 0) {
                if (!c->pit0_read) {
                    c->pit0_latch = pit0_count(c);
                    c->pit0_read  = 1;
                }
            } else {
                c->pit0_control = 0x80 | (*value & 0x3f);
                c->pit0_phase   = access == 2 ? 2 : 1;
                c->pit0_read    = 0;
            }
            result = 0;
        } else if (address == 0x40 && width == 1 && !write) {
            if (!c->pit0_read) {
                c->pit0_latch = pit0_count(c);
                c->pit0_read  = 1;
            }
            *value       = (c->pit0_latch >> (c->pit0_read == 2 ? 8 : 0)) & 255;
            c->pit0_read = c->pit0_read == 1 ? 2 : 0;
            result       = 0;
        } else if (address == 0x40 && width == 1 && write && c->pit0_phase) {
            unsigned access = (c->pit0_control >> 4) & 3;
            if (access == 3 && c->pit0_phase == 1) {
                c->pit0_low   = *value;
                c->pit0_phase = 2;
            } else {
                uint32_t reload = access == 1   ? (*value & 255)
                                  : access == 2 ? (*value & 255) << 8
                                                : c->pit0_low | ((*value & 255) << 8);
                c->pit0_reload  = reload ? reload : 65536;
                c->pit0_started = pit_ticks(c);
                if (access == 3)
                    c->pit0_phase = 1;
            }
            result = 0;
        } else if (address == 0x42 && width == 1 && !write && c->pit_read) {
            *value      = (c->pit_latch >> (c->pit_read == 2 ? 8 : 0)) & 255;
            c->pit_read = c->pit_read == 1 ? 2 : 0;
            result      = 0;
        } else if (address == 0x42 && width == 1 && write && c->pit_phase) {
            if (c->pit_phase == 1) {
                c->pit_low   = *value;
                c->pit_phase = 2;
            } else {
                c->pit_reload = c->pit_low | ((*value & 255) << 8);
                if (!c->pit_reload)
                    c->pit_reload = 65536;
                c->pit_started = pit_ticks(c);
                c->pit_phase   = 1;
            }
            result = 0;
        } else if (address == 0x61 && width == 1) {
            if (write) {
                if (!(c->port61 & 1) && (*value & 1))
                    c->pit_started = pit_ticks(c);
                c->port61 = *value & 3;
            } else {
                unsigned high = 1;
                if ((c->port61 & 1) && c->pit_reload)
                    high = pi_pit_elapsed(pit_ticks(c) - c->pit_started, c->pit_reload) < (c->pit_reload + 1) / 2;
                *value = c->port61 | (high << 5);
            }
            result = 0;
        } else if (!chipset_port(address) && !(address >= 0xcf8 && address < 0xd00) && address + width <= 0x10000 &&
                   !(address & (width - 1))) {
            /* Every other port is on the PCI bus, where the card decodes
             * what it owns (8514-style ports, a Trio64 BIOS's 32-bit write
             * to 8180h, a ViRGE's DDC port at E8h) and the bridge terminates
             * the rest. One aligned transaction of the guest's width. */
            uint32_t cpu = (uint32_t)c->io + address;
            if (write)
                transaction(cpu, width, 1, *value);
            else
                *value = transaction(cpu, width, 0, 0);
            result = 0;
        }
    } else if (op != X86_FETCH && legacy_range(address, width)) {
        if (c->diagnostics)
            ++c->diagnostics->vga_accesses;
        if (!c->legacy) {
            c->legacy_missed = 1;
            if (!write)
                *value = 0;
        } else {
            uint32_t cpu = (uint32_t)c->legacy + (address - PI_LEGACY_BASE);
            if (write)
                transaction(cpu, width, 1, *value);
            else
                *value = transaction(cpu, width, 0, 0);
        }
        result = 0;
    } else if (op != X86_FETCH && !(address & (width - 1))) {
        const struct PIBar *bar = find_bar(c, address, width);
        if (bar) {
            if (c->diagnostics)
                ++c->diagnostics->bar_accesses;
            if (c->bars_visible) {
                uint32_t cpu = bar->cpu + (address - bar->bus);
                if (write)
                    transaction(cpu, width, 1, *value);
                else
                    *value = transaction(cpu, width, 0, 0);
                result = 0;
            }
        }
    }
    pi_trace(c, op, address, width, *value);
    return result;
}
#ifdef PCIINIT_RECORD
#undef pi_hardware_access
uint32_t pi_hardware_access(PCIInitContext *c, uint32_t address, uint32_t width, uint32_t op, uint32_t *value)
{
    unsigned category = op >= X86_IN ? (address < 0x100 ? 0 : 1) : (address >= 0xa0000 && address < 0xc0000 ? 2 : 3);
    pi_profile_begin(category);
    uint32_t status = hardware_recorded_body(c, address, width, op, value);
    pi_profile_end(category);
    struct PIRecordAccess record = {address, width, op, *value, status};
    pi_record(PI_RECORD_ACCESS, &record, sizeof(record));
    return status;
}
#endif
static void bios_status(struct X86Registers *r, unsigned status)
{
    r->eax    = (r->eax & 0xffff00ffU) | (status << 8);
    r->eflags = (r->eflags & ~1U) | !!status;
}
uint32_t pi_bios(PCIInitContext *c, uint32_t number, struct X86Registers *r)
{
    unsigned function = r->eax & 65535;
    /* No previous VGA BIOS owns a display during this card's POST. Until
     * the ROM installs INT 10h (after which context.c chains to it), the
     * video services, also reached as INT 42h and 6Dh, keep the BIOS data
     * area consistent the way the system BIOS of a PC without a video ROM
     * would: mode and cursor state are recorded and reported, output is
     * discarded, and the video-addressing service reports unsupported
     * (AL != 12h) because there is no previous VGA BIOS to disable. */
    if (number == 0x10 || number == 0x42 || number == 0x6d) {
        unsigned f   = function >> 8;
        uint8_t *bda = c->ram + 0x400;
        if (f == 0) {
            uint8_t mode = function & 0x7f;
            bda[0x49]    = mode;
            bda[0x62]    = 0;
            r->eax       = (r->eax & 0xffff0000U) | (mode > 7 ? 0x20 : mode == 6 ? 0x3f : 0x30);
        } else if (f == 2) {
            unsigned page        = (r->ebx >> 8) & 7;
            bda[0x50 + page * 2] = r->edx & 255;
            bda[0x51 + page * 2] = (r->edx >> 8) & 255;
        } else if (f == 3) {
            unsigned page = (r->ebx >> 8) & 7;
            r->eax &= 0xffff0000U;
            r->ecx = (r->ecx & 0xffff0000U) | 0x0607;
            r->edx = (r->edx & 0xffff0000U) | bda[0x50 + page * 2] | ((uint32_t)bda[0x51 + page * 2] << 8);
        } else if (f == 5)
            bda[0x62] = function & 7;
        else if (f == 8)
            r->eax = (r->eax & 0xffff0000U) | 0x0720;
        else if (f == 0xf) {
            r->eax = (r->eax & 0xffff0000U) | 0x5000 | bda[0x49];
            r->ebx = (r->ebx & 0xffff00ffU) | ((uint32_t)bda[0x62] << 8);
        } else if (f == 0x12 && (r->ebx & 255) == 0x32)
            r->eax &= 0xffffff00U;
        else if (f != 1 && f != 6 && f != 7 && f != 9 && f != 0xa && f != 0xe)
            return 2;
        return 1;
    }
    /* The remaining services answer as a PC system BIOS does when nothing
     * is there: unsupported INT 15h functions (OEM hooks, memory queries)
     * return AH=86h with CF set, the keyboard reports no key waiting and
     * delivers a space when asked to wait for one, so a prompt cannot
     * hang, and the time-of-day count runs at 18.2 Hz from the PIT clock. */
    if (number == 0x15) {
        bios_status(r, 0x86);
        return 1;
    }
    if (number == 0x16) {
        unsigned f = function >> 8;
        if (f == 1 || f == 0x11) {
            r->eflags |= 0x40;
            r->eax &= 0xffff0000U;
        } else if (f == 0 || f == 0x10)
            r->eax = (r->eax & 0xffff0000U) | 0x3920;
        else if (f == 2 || f == 0x12)
            r->eax &= 0xffffff00U;
        return 1;
    }
    if (number == 0x1a && (function >> 8) == 0) {
        uint32_t count = c->timer ? (uint32_t)(pit_ticks(c) >> 16) : 0;
        r->ecx         = (r->ecx & 0xffff0000U) | (count >> 16);
        r->edx         = (r->edx & 0xffff0000U) | (count & 0xffff);
        bios_status(r, 0);
        return 1;
    }
    if (number == 0x1a && (function >> 8) == 1)
        return 1;
    if (number == 0x1a && (function >> 8) != 0xb1) {
        bios_status(r, 0x86);
        return 1;
    }
    if (number == 0x1a && (function >> 8) == 0xb1) {
        unsigned f = function & 255;
        if (f == 1) {
            r->eax = (r->eax & 0xffff0000U) | 1;
            r->edx = 0x20494350;
            r->ebx = 0x0210;
            r->ecx = c->bdf >> 8;
            bios_status(r, 0);
        } else if (f == 2 || f == 3) {
            int found = f == 2 ? ((r->edx & 65535) == c->vendor && (r->ecx & 65535) == c->device)
                               : ((r->ecx & 0xffffff) == (op_read(c->pci, c->board, 8, 4) >> 8));
            if (found && !(r->esi & 65535)) {
                r->ebx = c->bdf;
                bios_status(r, 0);
            } else
                bios_status(r, 0x86);
        } else if (f >= 8 && f <= 13) {
            unsigned width = 1U << ((f - 8) % 3), write = f >= 11;
            uint32_t value = r->ecx & (width == 1 ? 255U : width == 2 ? 65535U : 0xffffffffU);
            if (config(c, r->ebx & 65535, r->edi & 65535, width, write, &value))
                bios_status(r, 0x87);
            else {
                if (!write)
                    r->ecx = (r->ecx & (width == 1   ? 0xffffff00U
                                        : width == 2 ? 0xffff0000U
                                                     : 0)) |
                             (value & (width == 1   ? 255U
                                       : width == 2 ? 65535U
                                                    : 0xffffffffU));
                bios_status(r, 0);
            }
        } else
            bios_status(r, 0x81);
        return 1;
    }
    return 2;
}

/* String stores into the legacy window keep the guest's element width;
 * the card applies its own write mode to every element. One request
 * completes the prefix inside the window; later elements return in a new
 * request. Byte order follows transaction(). */
#ifdef PCIINIT_RECORD
#define pi_hardware_bulk hardware_bulk_body
static
#endif
    uint32_t pi_hardware_bulk(PCIInitContext *c, const struct X86BulkRequest *request, const uint8_t *source)
{
    struct X86BulkRequest r = *request;
    uint32_t bytes, i, room;
    volatile uint8_t *d;
    if (r.stride != (int32_t)r.width || !r.count || (r.width != 1 && r.width != 2 && r.width != 4) ||
        !legacy_range(r.destination, r.width))
        return 0;
    room = PI_LEGACY_BYTES - (r.destination - PI_LEGACY_BASE);
    if (r.width * r.count > room)
        r.count = room / r.width;
    bytes = r.width * r.count;
    d     = (volatile uint8_t *)c->legacy + (r.destination - PI_LEGACY_BASE);
    if (!c->legacy)
        c->legacy_missed = 1; /* dropped, see pi_hardware_access */
    else if (r.width == 1) {
        if (source)
            for (i = 0; i < bytes; ++i)
                d[i] = source[i];
        else
            for (i = 0; i < bytes; ++i)
                d[i] = r.value;
    } else if (r.width == 2) {
        /* RAM bytes are already in guest memory order; a 68k word store
         * of the first byte high keeps that order on the bus. */
        volatile uint16_t *p = (volatile uint16_t *)d;
        uint16_t v           = swap(r.value, 2);
        if (source)
            for (i = 0; i < r.count; ++i)
                p[i] = ((uint16_t)source[2 * i] << 8) | source[2 * i + 1];
        else
            for (i = 0; i < r.count; ++i)
                p[i] = v;
    } else {
        volatile uint32_t *p = (volatile uint32_t *)d;
        uint32_t v           = swap(r.value, 4);
        if (source)
            for (i = 0; i < r.count; ++i)
                p[i] = ((uint32_t)source[4 * i] << 24) | ((uint32_t)source[4 * i + 1] << 16) |
                       ((uint32_t)source[4 * i + 2] << 8) | source[4 * i + 3];
        else
            for (i = 0; i < r.count; ++i)
                p[i] = v;
    }
#ifdef PCIINIT_RECORD
    pi_record(PI_RECORD_BULK, &r, sizeof(r));
#endif
    if (c->diagnostics) {
        c->diagnostics->vga_accesses += r.count;
        for (i = 0; i < r.count; ++i) {
            uint32_t value = r.value, j;
            if (source) {
                value = 0;
                for (j = 0; j < r.width; ++j)
                    value |= (uint32_t)source[i * r.width + j] << (8 * j);
            } else if (r.width < 4)
                value &= (1U << (8 * r.width)) - 1;
            pi_trace(c, X86_WRITE, r.destination + i * r.width, r.width, value);
        }
    }
    return r.count;
}
#ifdef PCIINIT_RECORD
#undef pi_hardware_bulk
uint32_t pi_hardware_bulk(PCIInitContext *c, const struct X86BulkRequest *b, const uint8_t *source)
{
    pi_profile_begin(4);
    uint32_t done = hardware_bulk_body(c, b, source);
    pi_profile_end(4);
    return done;
}
#endif

/* REP SCAS over the legacy window: read elements until the comparison
 * ends the repeat, return how many were consumed and the final value. */
uint32_t pi_hardware_scan(PCIInitContext *c, const struct X86ScanRequest *s, uint32_t *last)
{
#ifdef PCIINIT_RECORD
    /* Recorded runs retain individual read events. */
    (void)c;
    (void)s;
    (void)last;
    return 0;
#else
    uint32_t i, value = 0, cpu;
    if (c->diagnostics || !s->count || s->stride != (int32_t)s->width ||
        (s->width != 1 && s->width != 2 && s->width != 4) || !legacy_range(s->address, s->width) ||
        s->width * s->count > PI_LEGACY_BYTES - (s->address - PI_LEGACY_BASE))
        return 0;
    cpu = (uint32_t)c->legacy + (s->address - PI_LEGACY_BASE);
    if (!c->legacy)
        c->legacy_missed = 1;
    for (i = 0; i < s->count;) {
        value = c->legacy ? transaction(cpu + i * s->width, s->width, 0, 0) : 0;
        ++i;
        if ((value == s->expected) != s->repeat_equal)
            break;
    }
    *last = value;
    return i;
#endif
}
