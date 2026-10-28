/* Guest-clock measurement of one cold S3 POST through the public API. */
#include <proto/exec.h>
#include <exec/execbase.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include <proto/pciinit.h>
#include "private/openpci_calls.h"
#include <stdio.h>
struct Library *PCIInitBase;
struct Device *TimerBase;
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct MsgPort *port      = NULL;
    struct timerequest *timer = NULL;
    struct Library *pci       = NULL;
    struct pci_dev *board     = NULL;
    PCIInitImage *image       = NULL;
    PCIInitContext *ctx       = NULL;
    struct EClockVal start, end;
    struct X86Limits limits        = {sizeof(limits), 10000000, 30000000, 3600000, NULL};
    struct TagItem tags[]          = {{PII_Hardware, 1}, {PII_Limits, (ULONG)&limits}, {TAG_DONE, 0}};
    struct PCIInitContextInfo info = {.size = sizeof(info)};
    struct PCIInitVBEInfo vbe      = {.size = sizeof(vbe)};
    uint32_t hz = 0, end_hz, status, command = 0;
    uint64_t ticks, microseconds;
    int failed = 1, opened = 0, quiesced = 0;
    port = CreateMsgPort();
    if (!port)
        goto out;
    timer = (struct timerequest *)CreateIORequest(port, sizeof(*timer));
    if (!timer)
        goto out;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer, 0))
        goto out;
    opened      = 1;
    TimerBase   = timer->tr_node.io_Device;
    PCIInitBase = OpenLibrary((STRPTR) "pciinit.library", PCIINIT_LIBRARY_VERSION);
    if (!PCIInitBase)
        goto out;
    pci = op_open(SysBase);
    if (!pci)
        goto out;
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    if (!board)
        goto out;
    command = op_read(pci, board, PCI_COMMAND, 2);
    image   = LoadOptionROMFromCard(board, NULL);
    if (!image)
        goto out;
    ctx = CreatePCIContext(board, tags);
    if (!ctx)
        goto out;
    /* Fresh isolated emulator card; the context owns it while preparing
     * the no-bus-master precondition required by RunOptionROM(). */
    op_write(pci, board, PCI_COMMAND, command & ~PCI_COMMAND_MASTER, 2);
    quiesced = 1;
    if (op_read(pci, board, PCI_COMMAND, 2) != (command & ~PCI_COMMAND_MASTER))
        goto out;
    printf("POST TIMING PCI original=%04lx prepared=%04lx\n", (ULONG)command, (ULONG)(command & ~PCI_COMMAND_MASTER));
    printf("POST TIMING CPU attn=%04lx caches=%08lx\n", (ULONG)SysBase->AttnFlags, (ULONG)CacheControl(0, 0));
    puts("POST TIMING begin: new context, card ROM loaded, diagnostic tracing disabled");
    fflush(stdout);
    hz     = ReadEClock(&start);
    status = RunOptionROM(ctx, image);
    end_hz = ReadEClock(&end);
    if (!hz || hz != end_hz)
        goto out;
    ticks        = (((uint64_t)end.ev_hi << 32) | end.ev_lo) - (((uint64_t)start.ev_hi << 32) | start.ev_lo);
    microseconds = ticks * 1000000 / hz;
    if (GetPCIContextInfo(ctx, &info) != PI_OK)
        goto out;
    printf(
        "POST TIMING status=%lu stop=%lu instructions=%lu accesses=%lu hz=%lu ticks_hi=%lu ticks_lo=%lu "
        "seconds=%lu.%06lu\n",
        (ULONG)status, (ULONG)info.execution.reason, (ULONG)info.execution.instructions, (ULONG)info.execution.accesses,
        (ULONG)hz, (ULONG)(ticks >> 32), (ULONG)ticks, (ULONG)(microseconds / 1000000),
        (ULONG)(microseconds % 1000000));
    fflush(stdout);
    if (status)
        printf("POST TIMING FAULT address=%08lx operation=%lu interrupt=%lu\n", (ULONG)info.execution.address,
               (ULONG)info.execution.operation, (ULONG)info.execution.interrupt);
    if (status != PI_OK || info.state != PI_INITIALIZED || info.execution.reason != X86_STOP_HALT)
        goto out;
    if (GetVBEInfo(ctx, &vbe) != PI_OK || vbe.mode_count != 25 || vbe.memory_bytes != 4194304)
        goto out;
    printf("POST TIMING VBE version=%04lx modes=%lu memory=%lu OEM=%s\n", (ULONG)vbe.version, (ULONG)vbe.mode_count,
           (ULONG)vbe.memory_bytes, vbe.oem);
    failed = 0;
out:
    if (quiesced) {
        if (op_read(pci, board, PCI_COMMAND, 2) != (command & ~PCI_COMMAND_MASTER))
            failed = 1;
        op_write(pci, board, PCI_COMMAND, command, 2);
    }
    if (ctx && DeletePCIContext(ctx) != PI_OK)
        failed = 1;
    if (image && FreeOptionROM(image) != PI_OK)
        failed = 1;
    if (pci && board) {
        if (op_read(pci, board, PCI_COMMAND, 2) != command || !op_obtain(pci, board))
            failed = 1;
        else
            op_release(pci, board);
    }
    if (pci)
        CloseLibrary(pci);
    if (PCIInitBase)
        CloseLibrary(PCIInitBase);
    if (opened)
        CloseDevice((struct IORequest *)timer);
    if (timer)
        DeleteIORequest((struct IORequest *)timer);
    if (port)
        DeleteMsgPort(port);
    printf("POST TIMING RESULT failures=%u\n", failed);
    return failed ? 20 : 0;
}
