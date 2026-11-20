/* Private diagnostic: POST, then use the installed ROM's text service.
 * No public BIOS/VBE API is introduced by this test executable. */
#include "private/pciinit.h"
#include <stdio.h>
static unsigned checks, failures;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct PIBase base          = {0};
    struct Library *pci         = NULL;
    struct pci_dev *board       = NULL;
    PCIInitImage *image         = NULL;
    PCIInitContext *ctx         = NULL;
    struct TagItem tags[]       = {{PII_Hardware, 1}, {TAG_DONE, 0}};
    static const char message[] = "pciinit.library POST OK";
    struct X86Limits limits     = {sizeof(limits), 100000, 1000000, 10000, NULL};
    struct X86Result result     = {.size = sizeof(result)};
    unsigned i, column = 0, row = 0;
    base.sys = SysBase;
    InitSemaphore(&base.vga_lock);
    base.engine = OpenLibrary((STRPTR) "x86emu.library", 1);
    CHECK(base.engine != NULL);
    if (!base.engine)
        goto out;
    pci = op_open(SysBase);
    CHECK(pci != NULL);
    if (!pci)
        goto out;
    while ((board = op_find(pci, board)))
        if ((op_read(pci, board, 8, 4) >> 24) == 3)
            break;
    CHECK(board != NULL);
    if (!board)
        goto out;
    image = pi_load_card(&base, board, NULL);
    CHECK(image != NULL);
    if (!image)
        goto out;
    ctx = pi_create(&base, board, tags);
    CHECK(ctx != NULL);
    if (!ctx)
        goto out;
    {
        uint32_t status = pi_run(&base, ctx, image);
        CHECK(status == PI_OK);
        if (status) {
            printf(
                "DISPLAY POST status=%lu stop=%lu instructions=%lu accesses=%lu address=%08lx op=%lu legacy=%08lx "
                "fault=%u\n",
                (ULONG)status, (ULONG)ctx->info.execution.reason, (ULONG)ctx->info.execution.instructions,
                (ULONG)ctx->info.execution.accesses, (ULONG)ctx->info.execution.address,
                (ULONG)ctx->info.execution.operation, (ULONG)ctx->legacy, ctx->legacy_missed);
            goto out;
        }
    }
    CHECK(ctx->ram[0x42] || ctx->ram[0x43]);
    if (failures)
        goto out;
    printf("DISPLAY POST mode=%u columns=%u INT10=%02x%02x:%02x%02x\n", ctx->ram[0x449], ctx->ram[0x44a],
           ctx->ram[0x43], ctx->ram[0x42], ctx->ram[0x41], ctx->ram[0x40]);
    CHECK(pi_hardware_start(ctx) == PI_OK);
    if (failures)
        goto out;
    /* The ROM's own banner leaves the cursor wherever it ends; the
     * teletype service must advance it by exactly the message length. */
    column           = ctx->ram[0x450];
    row              = ctx->ram[0x451];
    ctx->ram[0x7200] = 0xcd;
    ctx->ram[0x7201] = 0x10;
    ctx->ram[0x7202] = 0xf4;
    for (i = 0; message[i]; ++i) {
        struct X86Registers r = {.size   = sizeof(r),
                                 .eip    = 0x7200,
                                 .esp    = 0x6ff0,
                                 .eflags = 2,
                                 .eax    = 0x0e00 | (unsigned char)message[i],
                                 .ebx    = 7};
        CHECK(x86_set(base.engine, ctx->cpu, &r) == 0);
        CHECK(x86_run(base.engine, ctx->cpu, &limits, &result) == X86_STOP_HALT);
        x86_get(base.engine, ctx->cpu, &r);
        CHECK(r.segment[X86_CS] == 0 && r.eip == 0x7203);
        if (failures)
            break;
    }
    pi_hardware_finish(ctx);
    printf("DISPLAY text=%s\n", message);
    printf("DISPLAY cursor=%u,%u from %u,%u\n", ctx->ram[0x450], ctx->ram[0x451], column, row);
    CHECK(ctx->ram[0x450] == (column + sizeof(message) - 1) % 80 &&
          ctx->ram[0x451] == row + (column + sizeof(message) - 1) / 80);
out:
    if (ctx)
        CHECK(pi_delete(&base, ctx) == PI_OK);
    if (image)
        CHECK(pi_free_image(&base, image) == PI_OK);
    if (pci)
        CloseLibrary(pci);
    if (base.engine)
        CloseLibrary(base.engine);
    printf("DISPLAY RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
