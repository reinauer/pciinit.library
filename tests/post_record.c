/* Diagnostic recording only; resulting files contain the private card ROM. */
#include "private/pciinit.h"
#include "private/record.h"
#include <proto/timer.h>
#include <devices/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAPACITY (16U * 1024 * 1024)
static uint8_t *buffer;
static uint32_t used, records, overflow, hz;
static int profiling;
static struct
{
    struct EClockVal start, end;
    uint32_t samples, up, down, equal, min, max;
} waits[4];
static unsigned wait_count, read_high;
static uint32_t previous, low;
struct Device *TimerBase;
static struct EClockVal marks[6], access_start;
static uint64_t access_ticks[5];
static uint32_t access_calls[5];
void pi_profile_begin(unsigned category)
{
    (void)category;
    if (profiling)
        ReadEClock(&access_start);
}
void pi_profile_end(unsigned category)
{
    if (profiling) {
        struct EClockVal end;
        ReadEClock(&end);
        access_ticks[category] +=
            (((uint64_t)end.ev_hi << 32) | end.ev_lo) - (((uint64_t)access_start.ev_hi << 32) | access_start.ev_lo);
        ++access_calls[category];
    }
}
void pi_record_mark(unsigned phase)
{
    hz = ReadEClock(&marks[phase]);
}
void pi_record(uint32_t kind, const void *data, uint32_t bytes)
{
    uint32_t header[] = {kind, bytes};
    if (profiling) {
        ++records;
        if (kind == PI_RECORD_ACCESS && wait_count < 4) {
            const struct PIRecordAccess *r = data;
            if (r->operation == X86_OUT && r->address == 0x61) {
                if (r->value & 1) {
                    ReadEClock(&waits[wait_count].start);
                    waits[wait_count].min = 65536;
                    read_high             = 0;
                } else {
                    ReadEClock(&waits[wait_count].end);
                    ++wait_count;
                }
            } else if (r->operation == X86_IN && r->address == 0x42) {
                if (!read_high)
                    low = r->value;
                else {
                    uint32_t count = low | (r->value << 8);
                    if (waits[wait_count].samples++) {
                        if (count < previous)
                            ++waits[wait_count].down;
                        else if (count > previous)
                            ++waits[wait_count].up;
                        else
                            ++waits[wait_count].equal;
                    }
                    if (count < waits[wait_count].min)
                        waits[wait_count].min = count;
                    if (count > waits[wait_count].max)
                        waits[wait_count].max = count;
                    previous = count;
                }
                read_high = !read_high;
            }
        }
        return;
    }
    if (used > CAPACITY - 8 || bytes > CAPACITY - used - 8) {
        overflow = 1;
        return;
    }
    memcpy(buffer + used, header, 8);
    memcpy(buffer + used + 8, data, bytes);
    used += 8 + bytes;
    ++records;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct PIBase base      = {0};
    struct Library *pci     = NULL;
    struct pci_dev *board   = NULL;
    PCIInitImage *image     = NULL;
    PCIInitContext *ctx     = NULL;
    struct X86Limits limits = {sizeof(limits), 10000000, 30000000, 3600000, NULL};
    struct TagItem tags[]   = {{PII_Hardware, 1}, {PII_Limits, (ULONG)&limits}, {TAG_DONE, 0}};
    uint32_t magic = PI_RECORD_MAGIC, status = PI_BAD_STATE;
    struct MsgPort *port      = NULL;
    struct timerequest *timer = NULL;
    int failed = 1, opened = 0;
    FILE *file = NULL;
    if (argc != 2)
        goto out;
    profiling = !strcmp(argv[1], "--profile");
    if (!profiling && !(buffer = malloc(CAPACITY)))
        goto out;
    port = CreateMsgPort();
    if (!port)
        goto out;
    timer = (struct timerequest *)CreateIORequest(port, sizeof(*timer));
    if (!timer)
        goto out;
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer, 0))
        goto out;
    opened    = 1;
    TimerBase = timer->tr_node.io_Device;
    base.sys  = SysBase;
    InitSemaphore(&base.vga_lock);
    base.engine = OpenLibrary((STRPTR) "x86emu.library", 3);
    if (!base.engine || !(pci = op_open(SysBase)))
        goto out;
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    if (!board || !(image = pi_load_card(&base, board, NULL)) || !(ctx = pi_create(&base, board, tags)))
        goto out;
    status = pi_run(&base, ctx, image);
    if (status || overflow)
        goto out;
    if (profiling) {
        failed = 0;
        goto out;
    }
    file = fopen(argv[1], "wb");
    if (!file)
        goto out;
    if (fwrite(&magic, 4, 1, file) != 1 || fwrite(buffer, 1, used, file) != used)
        goto out;
    if (fclose(file)) {
        file = NULL;
        goto out;
    }
    file   = NULL;
    failed = 0;
out:
    if (file)
        fclose(file);
    if (ctx && pi_delete(&base, ctx))
        failed = 1;
    if (image && pi_free_image(&base, image))
        failed = 1;
    if (pci && board) {
        if (!op_obtain(pci, board))
            failed = 1;
        else
            op_release(pci, board);
    }
    if (pci)
        CloseLibrary(pci);
    if (base.engine)
        CloseLibrary(base.engine);
    if (opened)
        CloseDevice((struct IORequest *)timer);
    if (timer)
        DeleteIORequest((struct IORequest *)timer);
    if (port)
        DeleteMsgPort(port);
    if (!failed)
        for (unsigned i = 0; i < 6; i += 2) {
            uint64_t ticks = (((uint64_t)marks[i + 1].ev_hi << 32) | marks[i + 1].ev_lo) -
                             (((uint64_t)marks[i].ev_hi << 32) | marks[i].ev_lo);
            printf("RECORD PHASE %s us=%lu\n",
                   i == 0   ? "hardware-start"
                   : i == 2 ? "engine"
                            : "hardware-finish",
                   (ULONG)(ticks * 1000000 / hz));
        }
    if (profiling && !failed)
        for (unsigned i = 0; i < wait_count; ++i) {
            uint64_t ticks = (((uint64_t)waits[i].end.ev_hi << 32) | waits[i].end.ev_lo) -
                             (((uint64_t)waits[i].start.ev_hi << 32) | waits[i].start.ev_lo);
            printf("PROFILE PIT wait=%u us=%lu samples=%lu down=%lu up=%lu equal=%lu min=%lu max=%lu\n", i,
                   (ULONG)(ticks * 1000000 / hz), (ULONG)waits[i].samples, (ULONG)waits[i].down, (ULONG)waits[i].up,
                   (ULONG)waits[i].equal, (ULONG)waits[i].min, (ULONG)waits[i].max);
        }
    if (profiling && !failed)
        for (unsigned i = 0; i < 5; ++i) {
            static const char *const names[] = {"pit-io", "vga-io", "vga-memory", "other-memory", "vga-bulk"};
            printf("PROFILE ACCESS %s calls=%lu us=%lu\n", names[i], (ULONG)access_calls[i],
                   (ULONG)(access_ticks[i] * 1000000 / hz));
        }
    free(buffer);
    printf("%s RESULT status=%lu records=%lu bytes=%lu overflow=%lu failures=%u\n", profiling ? "PROFILE" : "RECORD",
           (ULONG)status, (ULONG)records, (ULONG)used, (ULONG)overflow, failed);
    return failed ? 20 : 0;
}
