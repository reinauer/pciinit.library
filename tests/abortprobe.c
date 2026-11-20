/* What the bus board does with a PCI cycle no device claims.
 *
 * On a PC a master abort ends with all-ones for a read and a dropped
 * write. An Amiga PCI bridge may do that, end the 68k cycle with a bus
 * error, return whatever is on the bus, or never end it. Exec hides the
 * answer: on the 68020/030 its bus error code completes the access itself
 * (a read gives zero) and on the 68040/060 it hangs when bit 8 of the
 * fault address is set. So for each probe access this program puts its
 * own handler on the bus error vector (tests/abortprobe_s.S), records the
 * frame if one arrives, and puts the vector back.
 *
 * It owns the first display controller, turns off its memory decode and
 * reads and writes its first memory BAR, then does the same with I/O
 * decode off at VGA port 3CCh (miscellaneous output, read-only). Each
 * access is first made with the decode on, as a control. Interrupts are
 * off from decode off to decode on, so no other driver touches the card
 * meanwhile. A bridge that never ends the cycle freezes the machine; the
 * line naming the access is printed before it starts.
 *
 * AbortProbe [FORCE]   FORCE: probe even if another driver owns the card
 */
#include <exec/execbase.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "private/openpci_calls.h"

#define AP_REG(type, name, reg) type name __asm(#reg)
uint32_t ap_read8(AP_REG(volatile void *, a, a0));
uint32_t ap_read16(AP_REG(volatile void *, a, a0));
uint32_t ap_read32(AP_REG(volatile void *, a, a0));
void ap_write8(AP_REG(volatile void *, a, a0), AP_REG(uint32_t, v, d0));
void ap_write16(AP_REG(volatile void *, a, a0), AP_REG(uint32_t, v, d0));
void ap_write32(AP_REG(volatile void *, a, a0), AP_REG(uint32_t, v, d0));
void ap_handler(void);
ULONG ap_vbr(void);
extern APTR ap_old;
extern uint32_t ap_faults, ap_frame_size;
extern uint16_t ap_frame[46];

#ifndef AFB_68060
#define AFB_68060 7
#endif

static struct Library *pci;
static struct pci_dev *board;
static volatile APTR *vector;
/* Per space (0 memory, 1 I/O): faults, all-ones reads, other reads. */
static unsigned bus_errors[2], all_ones[2], other_values[2], problems;

static uint32_t frame_long(unsigned offset)
{
    return ((uint32_t)ap_frame[offset / 2] << 16) | ap_frame[offset / 2 + 1];
}
/* The status word and fault address of the recorded frame. */
static void print_frame(void)
{
    unsigned format = ap_frame[3] >> 12, i;
    printf("    bus error: format $%X vector offset $%03X PC %08lx", format, ap_frame[3] & 0xfff,
           (unsigned long)frame_long(2));
    if (format == 0xa || format == 0xb)
        printf(" SSW %04x address %08lx", ap_frame[5], (unsigned long)frame_long(16));
    else if (format == 7)
        printf(" SSW %04x address %08lx", ap_frame[6], (unsigned long)frame_long(20));
    else if (format == 4)
        printf(" FSLW %08lx address %08lx", (unsigned long)frame_long(12), (unsigned long)frame_long(8));
    printf("\n    frame:");
    for (i = 0; i < ap_frame_size / 2; ++i)
        printf("%s %04x", i && !(i % 16) ? "\n          " : "", ap_frame[i]);
    printf("\n");
}

/* One access with the bus error vector pointing to the probe. command is
 * written first and the owned value restored after, with interrupts off
 * in between. Returns the value read and sets *faulted. */
static uint32_t access(uint32_t command, uint32_t owned, volatile void *address, unsigned width, int write,
                       uint32_t value, int *faulted)
{
    uint32_t result = 0, before;
    Disable();
    ap_old  = *vector;
    *vector = (APTR)ap_handler;
    CacheClearU();
    before = ap_faults;
    op_write(pci, board, PCI_COMMAND, command, 2);
    if (write && width == 1)
        ap_write8(address, value);
    else if (write && width == 2)
        ap_write16(address, value);
    else if (write)
        ap_write32(address, value);
    else if (width == 1)
        result = ap_read8(address);
    else if (width == 2)
        result = ap_read16(address);
    else
        result = ap_read32(address);
    op_write(pci, board, PCI_COMMAND, owned, 2);
    *faulted = ap_faults != before;
    if (*vector == (APTR)ap_handler)
        *vector = ap_old;
    CacheClearU();
    Enable();
    return result;
}

static void probe(const char *what, unsigned space, uint32_t command, uint32_t owned, uint32_t address,
                  unsigned width, int write, int control)
{
    int faulted;
    uint32_t mask = width == 4 ? 0xffffffffU : (1U << (8 * width)) - 1, value;
    printf("%-22s %s%u at %08lx ... ", what, write ? "write" : "read", 8 * width, (unsigned long)address);
    fflush(stdout);
    Flush(Output());
    Delay(5); /* let the console show the line before a possible freeze */
    value = access(command, owned, (volatile void *)address, width, write, 0x5a5a5a5aU & mask, &faulted);
    if (faulted) {
        printf("BUS ERROR\n");
        print_frame();
        if (control)
            ++problems;
        else
            ++bus_errors[space];
    } else if (write)
        printf("completed\n");
    else {
        printf("%0*lx\n", width * 2, (unsigned long)value);
        if (!control && (value & mask) == mask)
            ++all_ones[space];
        else if (!control)
            ++other_values[space];
    }
}

/* What the aborted cycles of one space did. */
static const char *verdict(unsigned space)
{
    if (problems)
        return "inconclusive";
    if (bus_errors[space])
        return all_ones[space] || other_values[space] ? "mixed" : "bus-error";
    if (other_values[space])
        return all_ones[space] ? "mixed" : "other-value";
    return all_ones[space] ? "all-ones" : "not-probed";
}

int main(int argc, char **argv)
{
    struct ExecBase *sys = SysBase;
    uint32_t command, owned, io, memory = 0, vbr;
    unsigned i;
    int force = argc > 1 && !strcmp(argv[1], "FORCE"), owner = 0;
    /* Without 68060.library exec reports a 68060 as a 68040. */
    printf("AbortProbe: CPU %s, ", (sys->AttnFlags & (1 << AFB_68060)) ? "68060"
                                   : (sys->AttnFlags & AFF_68040)       ? "68040 or 68060"
                                   : (sys->AttnFlags & AFF_68030)       ? "68030"
                                                                        : "68020");
    pci = op_open(sys);
    if (!pci) {
        printf("no openpci.library\n");
        return 20;
    }
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    if (!board) {
        printf("no display controller\n");
        CloseLibrary(pci);
        return 20;
    }
    owner = op_obtain(pci, board);
    printf("card %04x:%04x at %lu:%lu.%lu\n", board->vendor, board->device,
           (unsigned long)op_attr(pci, board, PRM_BusNumber), (unsigned long)op_attr(pci, board, PRM_SlotNumber),
           (unsigned long)op_attr(pci, board, PRM_FunctionNumber));
    if (!owner && !force) {
        printf("another driver owns the card; run with FORCE to probe anyway\n");
        CloseLibrary(pci);
        return 10;
    }
    for (i = 0; i < 6 && !memory; ++i) {
        uint32_t bar  = op_read(pci, board, 0x10 + 4 * i, 4);
        uint32_t addr = op_attr(pci, board, PRM_MemoryAddr0 + i);
        if (!(bar & 1) && addr && addr != 0xffffffffU && op_attr(pci, board, PRM_MemorySize0 + i))
            memory = addr;
    }
    io      = op_attr(pci, board, PRM_LegacyIOSpace);
    vbr     = Supervisor((ULONG (*)())(APTR)ap_vbr);
    vector  = (volatile APTR *)(vbr + 8);
    command = op_read(pci, board, PCI_COMMAND, 2);
    owned   = (command | PCI_COMMAND_IO | PCI_COMMAND_MEMORY) & ~PCI_COMMAND_MASTER;
    printf("command %04lx, memory BAR at %08lx, I/O at %08lx, bus error vector at %08lx -> %08lx\n",
           (unsigned long)command, (unsigned long)memory, (unsigned long)io, (unsigned long)vector,
           (unsigned long)*vector);
    op_write(pci, board, PCI_COMMAND, owned, 2);
    if (memory) {
        probe("memory decode on:", 0, owned, owned, memory, 4, 0, 1);
        probe("memory decode off:", 0, owned & ~PCI_COMMAND_MEMORY, owned, memory, 4, 0, 0);
        probe("memory decode off:", 0, owned & ~PCI_COMMAND_MEMORY, owned, memory, 1, 0, 0);
        probe("memory decode off:", 0, owned & ~PCI_COMMAND_MEMORY, owned, memory + 0x100, 4, 0, 0);
        probe("memory decode off:", 0, owned & ~PCI_COMMAND_MEMORY, owned, memory + 0x10, 4, 1, 0);
        probe("memory decode on:", 0, owned, owned, memory, 4, 0, 1);
    } else
        printf("no memory BAR to probe\n");
    if (io) {
        probe("I/O decode on:", 1, owned, owned, io + 0x3cc, 1, 0, 1);
        probe("I/O decode off:", 1, owned & ~PCI_COMMAND_IO, owned, io + 0x3cc, 1, 0, 0);
        probe("I/O decode off:", 1, owned & ~PCI_COMMAND_IO, owned, io + 0x3cc, 1, 1, 0);
        probe("I/O decode on:", 1, owned, owned, io + 0x3cc, 1, 0, 1);
    } else
        printf("no I/O window to probe\n");
    op_write(pci, board, PCI_COMMAND, command, 2);
    if (owner)
        op_release(pci, board);
    CloseLibrary(pci);
    printf("ABORTPROBE RESULT memory=%s io=%s bus_errors=%u/%u all_ones=%u/%u other=%u/%u control_faults=%u\n",
           verdict(0), verdict(1), bus_errors[0], bus_errors[1], all_ones[0], all_ones[1], other_values[0],
           other_values[1], problems);
    return problems ? 10 : 0;
}
