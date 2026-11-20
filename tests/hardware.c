/* Native owned-card policy checks; never execute a ROM or submit DMA. */
#include "private/pciinit.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
static void wr(PCIInitContext *c, unsigned port, uint8_t index, uint8_t data)
{
    uint32_t value = index | ((uint32_t)data << 8);
    CHECK(pi_hardware_access(c, port, 2, X86_OUT, &value) == 0);
}
static uint8_t rd(volatile uint8_t *io, unsigned port, uint8_t index)
{
    io[port] = index;
    return io[port + 1];
}
/* A master abort: with memory decode off the card ignores its own BAR.
 * PC bridges answer all-ones and drop the write. A bridge that ends the
 * Amiga cycle with a bus error instead (WinUAE's masterabort bridge option)
 * meets exec first: on the 68020/030 exec completes a data fault itself and
 * the read returns zero; on the 68040/060 it reaches the library's trap
 * handler, which must give the PC values, count the faults and stop the
 * run in strict mode. */
static void master_abort_case(PCIInitContext *c, struct Library *pci, struct pci_dev *board, uint32_t command,
                              uint32_t bus_error)
{
    uint32_t caught   = bus_error && (SysBase->AttnFlags & AFF_68040);
    uint32_t expected = bus_error && !caught ? 0 : 0xffffffffU;
    uint32_t v, read, owned = (command | 3) & ~PCI_COMMAND_MASTER;
    c->hardware = 1;
    CHECK(pi_hardware_start(c) == PI_OK);
    if (failures)
        return;
    CHECK(c->bars_visible);
    op_write(pci, board, PCI_COMMAND, owned & ~PCI_COMMAND_MEMORY, 2);
    read = 1;
    CHECK(pi_hardware_access(c, c->bars[0].bus, 4, X86_READ, &read) == 0 && read == expected);
    v = 0x12345678;
    CHECK(pi_hardware_access(c, c->bars[0].bus + 4, 4, X86_WRITE, &v) == 0);
    CHECK(c->trap.faults == (caught ? 2 : 0));
    printf("HARDWARE master abort read=%08lx faults=%lu address=%08lx bar=%08lx\n", (ULONG)read, (ULONG)c->trap.faults,
           (ULONG)c->trap.address, (ULONG)c->bars[0].cpu);
    c->faults_seen    = c->trap.faults;
    c->stop_bus_error = 1;
    pi_hardware_flags(c);
    v = 1;
    CHECK(pi_hardware_access(c, c->bars[0].bus, 2, X86_READ, &v) == caught && (v & 0xffff) == (expected & 0xffff));
    c->stop_bus_error = 0;
    pi_hardware_flags(c);
    op_write(pci, board, PCI_COMMAND, owned, 2);
    CHECK(pi_hardware_access(c, c->bars[0].bus, 4, X86_READ, &v) == 0);
    /* Turned off by the ROM's own configuration write, the decode is
     * known and the abort never reaches the bus: all-ones on every CPU. */
    v = 0x80000000U | (c->bdf << 8) | PCI_COMMAND;
    CHECK(pi_hardware_access(c, 0xcf8, 4, X86_OUT, &v) == 0);
    v = PCI_COMMAND_IO;
    CHECK(pi_hardware_access(c, 0xcfc, 2, X86_OUT, &v) == 0);
    v    = c->trap.faults;
    read = 1;
    CHECK(pi_hardware_access(c, c->bars[0].bus, 4, X86_READ, &read) == 0 && read == 0xffffffffU && c->trap.faults == v);
    v = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    CHECK(pi_hardware_access(c, 0xcfc, 2, X86_OUT, &v) == 0);
    op_write(pci, board, PCI_COMMAND, owned, 2);
    c->info.warnings   = 0;
    c->info.bus_errors = 0;
    pi_hardware_finish(c);
    CHECK(c->info.bus_errors == (caught ? 3 : 0));
    CHECK(!(c->info.warnings & PI_WARN_BUS_ERROR) == !caught);
    CHECK(!caught || c->info.bus_error_address == c->bars[0].cpu);
    CHECK(op_read(pci, board, PCI_COMMAND, 2) == command);
}
/* Legacy VGA memory through the bridge window: sequencer map mask and
 * read map select give a planar round trip any VGA chip performs. */
static void legacy_case(PCIInitContext *c)
{
    volatile uint8_t *io = (volatile uint8_t *)c->io;
    uint8_t seq2 = rd(io, 0x3c4, 2), seq4 = rd(io, 0x3c4, 4), gc4 = rd(io, 0x3ce, 4), gc5 = rd(io, 0x3ce, 5),
            gc6 = rd(io, 0x3ce, 6);
    uint8_t gc1 = rd(io, 0x3ce, 1), gc3 = rd(io, 0x3ce, 3), gc8 = rd(io, 0x3ce, 8);
    uint32_t value, saved[2], i, count = c->diagnostics->count, accesses = c->diagnostics->vga_accesses;
    struct PCIInitTraceEntry *e;
    CHECK(c->legacy != NULL);
    if (!c->legacy)
        return;
    wr(c, 0x3c4, 4, 6);
    wr(c, 0x3ce, 5, 0);
    wr(c, 0x3ce, 6, 5);
    wr(c, 0x3ce, 1, 0);
    wr(c, 0x3ce, 3, 0);
    wr(c, 0x3ce, 8, 255);
    for (i = 0; i < 2; ++i) {
        wr(c, 0x3ce, 4, i);
        wr(c, 0x3c4, 2, 1U << i);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_READ, &value));
        saved[i] = value;
    }
    wr(c, 0x3c4, 2, 1);
    value = 0x5a;
    CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_WRITE, &value));
    wr(c, 0x3c4, 2, 2);
    value = 0xa5;
    CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_WRITE, &value));
    wr(c, 0x3ce, 4, 0);
    value = 0;
    CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_READ, &value) && value == 0x5a);
    wr(c, 0x3ce, 4, 1);
    value = 0;
    CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_READ, &value) && value == 0xa5);
    CHECK(c->diagnostics->vga_accesses == accesses + 6);
    e = &c->diagnostics->entries[(c->diagnostics->count - 1) % c->diagnostics->capacity];
    CHECK(e->operation == X86_READ && e->address == 0xa0000 && e->width == 1 && e->value == 0xa5);
    /* Word and dword element order through the window: a little-endian
     * guest value keeps its low byte at the lower address. */
    {
        struct X86BulkRequest b       = {sizeof(b), X86_BULK_FILL, 0, 0xa0010, 2, 2, 0x3412, 2};
        static const uint8_t source[] = {0x11, 0x22, 0x33, 0x44};
        wr(c, 0x3c4, 2, 1);
        wr(c, 0x3ce, 4, 0);
        count = c->diagnostics->count;
        CHECK(pi_hardware_bulk(c, &b, NULL) == 2);
        CHECK(c->diagnostics->count == count + 2);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0010, 1, X86_READ, &value) && value == 0x12);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0013, 1, X86_READ, &value) && value == 0x34);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0010, 2, X86_READ, &value) && value == 0x3412);
        b.operation = X86_BULK_COPY;
        b.width     = 4;
        b.count     = 1;
        b.stride    = 4;
        CHECK(pi_hardware_bulk(c, &b, source) == 1);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0010, 4, X86_READ, &value) && value == 0x44332211);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0011, 1, X86_READ, &value) && value == 0x22);
        b.destination = 0xa0014;
        b.width       = 2;
        b.count       = 2;
        b.stride      = 2;
        CHECK(pi_hardware_bulk(c, &b, source) == 2);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0014, 4, X86_READ, &value) && value == 0x44332211);
        value = 0;
        CHECK(!pi_hardware_access(c, 0xa0016, 1, X86_READ, &value) && value == 0x33);
        b.destination = 0xa0010;
        b.width       = 4;
        b.count       = 1;
        b.stride      = 4;
        b.destination = 0xbfffe;
        b.count       = 4;
        b.width       = 1;
        b.stride      = 1;
        b.operation   = X86_BULK_FILL;
        b.value       = 7;
        CHECK(pi_hardware_bulk(c, &b, NULL) == 2); /* clamped to the window end */
        b.destination = 0xc0000;
        CHECK(pi_hardware_bulk(c, &b, NULL) == 0);
    }
    {
        struct X86ScanRequest s = {sizeof(s), 0xa0010, 1, 4, 0x11, 1, 1};
        uint32_t last           = 0;
        CHECK(pi_hardware_scan(c, &s, &last) == 0); /* diagnostics keep scalar reads */
        c->diagnostics = NULL;
        CHECK(pi_hardware_scan(c, &s, &last) == 2 && last == 0x22);
        s.repeat_equal = 0;
        s.expected     = 0x11;
        CHECK(pi_hardware_scan(c, &s, &last) == 1 && last == 0x11);
        s.address = 0xbfffc;
        s.count   = 8;
        CHECK(pi_hardware_scan(c, &s, &last) == 0);
    }
    for (i = 0; i < 2; ++i) {
        wr(c, 0x3c4, 2, 1U << i);
        value = saved[i];
        CHECK(!pi_hardware_access(c, 0xa0000, 1, X86_WRITE, &value));
    }
    wr(c, 0x3ce, 1, gc1);
    wr(c, 0x3ce, 3, gc3);
    wr(c, 0x3ce, 8, gc8);
    wr(c, 0x3ce, 4, gc4);
    wr(c, 0x3ce, 5, gc5);
    wr(c, 0x3ce, 6, gc6);
    wr(c, 0x3c4, 2, seq2);
    wr(c, 0x3c4, 4, seq4);
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct PIBase base    = {0};
    PCIInitContext c      = {0};
    struct pci_dev *board = NULL;
    struct Library *pci   = op_open(SysBase);
    struct X86Registers r = {.size = sizeof(r)};
    struct PCIInitTraceEntry entries[4];
    struct PCIInitDiagnostics diag = {.size = sizeof(diag), .capacity = 4, .entries = entries};
    uint32_t v, bar, command, id;
    CHECK(pci != NULL);
    if (!pci)
        return 20;
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    CHECK(board != NULL);
    if (!board)
        goto out;
    CHECK(op_obtain(pci, board));
    if (failures)
        goto out;
    base.sys      = SysBase;
    c.owner       = &base;
    c.pci         = pci;
    c.board       = board;
    c.vendor      = board->vendor;
    c.device      = board->device;
    c.diagnostics = &diag;
    id            = ((uint32_t)c.device << 16) | c.vendor;
    bar           = op_read(pci, board, 0x10, 4);
    command       = op_read(pci, board, 4, 2);
    CHECK(pi_hardware_start(&c) == PI_UNSUPPORTED_MAPPING);
    /* "masterabort": the bridge raises a bus error for unclaimed cycles. */
    master_abort_case(&c, pci, board, command, argc > 1 && !strcmp(argv[1], "masterabort"));
    c.hardware = 1;
    CHECK(pi_hardware_start(&c) == PI_OK);
    if (failures)
        goto release;
    printf("HARDWARE bus=%lu legacy=%08lx bars=%u visible=%u\n", op_bus(pci), (ULONG)c.legacy, c.bar_count,
           c.bars_visible);
    CHECK(c.bar_count >= 1 && c.bars[0].bus == (bar & ~15U));
    v = 0x80000000U | (c.bdf << 8);
    CHECK(pi_hardware_access(&c, 0xcf8, 4, X86_OUT, &v) == 0);
    v = 0;
    CHECK(pi_hardware_access(&c, 0xcfc, 4, X86_IN, &v) == 0 && v == id);
    v = 0x80000004U | (c.bdf << 8);
    CHECK(!pi_hardware_access(&c, 0xcf8, 4, X86_OUT, &v));
    v = 7;
    CHECK(pi_hardware_access(&c, 0xcfc, 2, X86_OUT, &v) != 0);
    CHECK(!(op_read(pci, board, 4, 2) & 4));
    v = 0x80000010U | (c.bdf << 8);
    CHECK(!pi_hardware_access(&c, 0xcf8, 4, X86_OUT, &v));
    v = 0xffffffff;
    CHECK(pi_hardware_access(&c, 0xcfc, 4, X86_OUT, &v) != 0);
    CHECK(op_read(pci, board, 0x10, 4) == bar);
    /* Device memory is data, never code; unaligned BAR accesses are refused. */
    CHECK(pi_hardware_access(&c, c.bars[0].bus, 1, X86_FETCH, &v) != 0);
    CHECK(pi_hardware_access(&c, c.bars[0].bus + 1, 2, X86_READ, &v) != 0);
    CHECK(pi_hardware_access(&c, 0x70, 1, X86_OUT, &v) != 0);
    /* Chipset ports stay local; every other port reaches the PCI bus as
     * one aligned transaction, whether or not the card decodes it. */
    CHECK(pi_hardware_access(&c, 0x21, 1, X86_OUT, &v) != 0);
    v = 0;
    CHECK(!pi_hardware_access(&c, 0x8180, 4, X86_OUT, &v));
    v = 0;
    CHECK(!pi_hardware_access(&c, 0x8180, 4, X86_IN, &v));
    v = 0;
    CHECK(!pi_hardware_access(&c, 0xe8, 1, X86_IN, &v));
    CHECK(pi_hardware_access(&c, 0x8181, 2, X86_IN, &v) != 0);
    CHECK(pi_hardware_access(&c, 0xfffe, 4, X86_IN, &v) != 0);
    r.eax = 0xb101;
    CHECK(pi_bios(&c, 0x1a, &r) == 1);
    CHECK(!(r.eflags & 1) && r.edx == 0x20494350 && (r.eax & 255) == 1);
    r.eax = 0xb102;
    r.edx = c.vendor;
    r.ecx = c.device;
    r.esi = 0;
    CHECK(pi_bios(&c, 0x1a, &r) == 1 && !(r.eflags & 1) && r.ebx == c.bdf);
    r.eax = 0xb102;
    r.esi = 1;
    CHECK(pi_bios(&c, 0x1a, &r) == 1 && (r.eflags & 1) && ((r.eax >> 8) & 255) == 0x86);
    r.eax = 0xb10d;
    r.ebx = c.bdf;
    r.edi = 0x10;
    r.ecx = 0xffffffff;
    CHECK(pi_bios(&c, 0x1a, &r) == 1 && (r.eflags & 1));
    CHECK(op_read(pci, board, 0x10, 4) == bar);
    r.eax = 0x1201;
    r.ebx = 0x32;
    CHECK(pi_bios(&c, 0x10, &r) == 1 && (r.eax & 255) != 0x12);
    CHECK(pi_bios(&c, 0x13, &r) == 2);
    /* Bootstrap video services keep the BIOS data area: mode, cursor, page. */
    r.eax = 0x0003;
    CHECK(pi_bios(&c, 0x42, &r) == 1 && c.ram[0x449] == 3 && (r.eax & 255) == 0x30);
    r.eax = 0x0200;
    r.ebx = 0x0100;
    r.edx = 0x0a05;
    CHECK(pi_bios(&c, 0x10, &r) == 1);
    r.eax = 0x0300;
    r.ebx = 0x0100;
    r.edx = 0;
    CHECK(pi_bios(&c, 0x6d, &r) == 1 && (r.edx & 0xffff) == 0x0a05 && (r.ecx & 0xffff) == 0x0607);
    r.eax = 0x0f00;
    r.ebx = 0;
    CHECK(pi_bios(&c, 0x10, &r) == 1 && (r.eax & 0xffff) == 0x5003);
    r.eax = 0x1b00;
    CHECK(pi_bios(&c, 0x10, &r) == 2);
    /* System BIOS stand-ins: unsupported INT 15h, empty keyboard, ticks, INT 42h. */
    r.eax    = 0xf965;
    r.eflags = 0;
    CHECK(pi_bios(&c, 0x15, &r) == 1 && (r.eflags & 1) && ((r.eax >> 8) & 255) == 0x86);
    r.eax    = 0x0100;
    r.eflags = 0;
    CHECK(pi_bios(&c, 0x16, &r) == 1 && (r.eflags & 0x40) && (r.eax & 0xffff) == 0);
    r.eax = 0x0000;
    CHECK(pi_bios(&c, 0x16, &r) == 1 && (r.eax & 0xffff) == 0x3920);
    r.eax = 0x0000;
    r.ecx = r.edx = 0xffffffffU;
    r.eflags      = 1;
    CHECK(pi_bios(&c, 0x1a, &r) == 1 && !(r.eflags & 1) && (r.ecx >> 16) == 0xffff && (r.edx >> 16) == 0xffff);
    r.eax    = 0x0200;
    r.eflags = 0;
    CHECK(pi_bios(&c, 0x1a, &r) == 1 && (r.eflags & 1));
    CHECK(diag.count > 4 && diag.io_accesses > 0);
    v = 0;
    CHECK(!pi_hardware_access(&c, 0x61, 1, X86_OUT, &v));
    v = 0xb6;
    CHECK(!pi_hardware_access(&c, 0x43, 1, X86_OUT, &v));
    v = 0x35;
    CHECK(!pi_hardware_access(&c, 0x42, 1, X86_OUT, &v));
    v = 0x12;
    CHECK(!pi_hardware_access(&c, 0x42, 1, X86_OUT, &v));
    v = 0x80;
    CHECK(!pi_hardware_access(&c, 0x43, 1, X86_OUT, &v));
    CHECK(!pi_hardware_access(&c, 0x42, 1, X86_IN, &v) && v == 0x35);
    CHECK(!pi_hardware_access(&c, 0x42, 1, X86_IN, &v) && v == 0x12);
    CHECK(!pi_hardware_access(&c, 0x61, 1, X86_IN, &v) && (v & 0x20));
    /* DMA page registers: word and byte POST codes on 84h land in latches
     * that read back at any width; the block ends at 8Fh. */
    v = 0;
    CHECK(!pi_hardware_access(&c, 0x84, 2, X86_OUT, &v));
    v = 0x5a;
    CHECK(!pi_hardware_access(&c, 0x84, 1, X86_OUT, &v));
    v = 0xa5;
    CHECK(!pi_hardware_access(&c, 0x85, 1, X86_OUT, &v));
    CHECK(!pi_hardware_access(&c, 0x84, 2, X86_IN, &v) && v == 0xa55a);
    CHECK(!pi_hardware_access(&c, 0x80, 1, X86_IN, &v) && v == 0);
    v = 0x11223344;
    CHECK(!pi_hardware_access(&c, 0x8c, 4, X86_OUT, &v));
    CHECK(!pi_hardware_access(&c, 0x8e, 2, X86_IN, &v) && v == 0x1122);
    v = 0;
    CHECK(pi_hardware_access(&c, 0x8e, 4, X86_OUT, &v));
    /* Channel 0: latch and read the free-running system tick, reprogram it
     * with a low/high reload, and read without a latch. */
    v = 0x00;
    CHECK(!pi_hardware_access(&c, 0x43, 1, X86_OUT, &v));
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &v) && v < 256);
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &v) && v < 256);
    v = 0x36;
    CHECK(!pi_hardware_access(&c, 0x43, 1, X86_OUT, &v));
    v = 0x34;
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_OUT, &v));
    v = 0x12;
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_OUT, &v));
    v = 0x00;
    CHECK(!pi_hardware_access(&c, 0x43, 1, X86_OUT, &v));
    {
        uint32_t low = 0, high = 0;
        CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &low));
        CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &high));
        CHECK((high << 8 | low) <= 0x1234);
    }
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &v) && v < 256);
    CHECK(!pi_hardware_access(&c, 0x40, 1, X86_IN, &v) && v < 256);
    /* The read-back command and channel 1 stay unanswered. */
    v = 0xc2;
    CHECK(pi_hardware_access(&c, 0x43, 1, X86_OUT, &v) != 0);
    v = 0x76;
    CHECK(pi_hardware_access(&c, 0x43, 1, X86_OUT, &v) != 0);
    legacy_case(&c);
    c.diagnostics = &diag;
    pi_hardware_finish(&c);
    CHECK(c.timer == NULL && c.timer_port == NULL && c.legacy == NULL);
    CHECK(op_read(pci, board, 4, 2) == command && op_read(pci, board, 0x10, 4) == bar);
    /* An enabled command bit does not imply a running DMA engine. Exercise
     * both initial states on this idle, exclusively owned fixture card. */
    for (unsigned master = 0; master < 2; ++master) {
        uint32_t initial = (command & ~PCI_COMMAND_MASTER) | (master ? PCI_COMMAND_MASTER : 0);
        op_write(pci, board, PCI_COMMAND, initial, 2);
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == initial);
        v = pi_hardware_start(&c);
        CHECK(v == PI_OK);
        if (v == PI_OK) {
            CHECK(op_read(pci, board, PCI_COMMAND, 2) == ((initial | 3) & ~PCI_COMMAND_MASTER));
            pi_hardware_finish(&c);
        }
        CHECK(op_read(pci, board, PCI_COMMAND, 2) == initial);
    }
    op_write(pci, board, PCI_COMMAND, command, 2);
    /* Without the window the ROM runs on: reads zero, writes vanish, flagged. */
    c.hardware = 1;
    CHECK(pi_hardware_start(&c) == PI_OK);
    if (c.legacy) {
        op_unregion(pci, board, c.legacy);
        c.legacy = NULL;
    }
    v = 0x55;
    CHECK(pi_hardware_access(&c, 0xa0000, 1, X86_WRITE, &v) == 0 && c.legacy_missed);
    c.legacy_missed = 0;
    v               = 0x55;
    CHECK(pi_hardware_access(&c, 0xa0000, 2, X86_READ, &v) == 0 && v == 0 && c.legacy_missed);
    {
        struct X86BulkRequest b = {sizeof(b), X86_BULK_FILL, 0, 0xbff00, 2, 0x100, 0x1234, 2};
        struct X86ScanRequest s = {sizeof(s), 0xa0000, 4, 8, 0, 1, 4};
        uint32_t last           = 1;
        c.legacy_missed         = 0;
        CHECK(pi_hardware_bulk(&c, &b, NULL) == 0x80 && c.legacy_missed);
        c.diagnostics   = NULL; /* scans stay scalar while tracing */
        c.legacy_missed = 0;
        CHECK(pi_hardware_scan(&c, &s, &last) == 8 && last == 0 && c.legacy_missed);
        c.diagnostics = &diag;
    }
    CHECK(!(c.info.warnings & PI_WARN_NO_LEGACY_WINDOW));
    pi_hardware_finish(&c);
    CHECK(c.info.warnings & PI_WARN_NO_LEGACY_WINDOW);
release:
    op_release(pci, board);
out:
    if (pci)
        CloseLibrary(pci);
    printf("HARDWARE RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
