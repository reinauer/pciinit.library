/* Real timer.device request lifecycle; run in WinUAE or on AmigaOS. */
#include "private/backend.h"
#include <exec/execbase.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdio.h>
static uint8_t code[] = {0xeb, 0xfe, 0xf4};
static unsigned checks, failures;
#define CHECK(c)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(c)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #c); \
        }                                          \
    } while (0)
static uint32_t X86_CALLBACK memory(void *u, uint32_t a, uint32_t w, uint32_t op, uint32_t *v)
{
    (void)u;
    if (op != X86_FETCH || a > sizeof(code) || w > sizeof(code) - a)
        return 1;
    *v = 0;
    for (unsigned i = 0; i < w; ++i)
        *v |= (uint32_t)code[a + i] << (8 * i);
    return 0;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(void)
{
    struct X86CodeRegion region = {0, sizeof(code), code};
    struct X86Config config     = {
        .size = sizeof(config), .version = X86EMU_ABI_VERSION, .access = memory, .region_count = 1, .regions = &region};
    X86Context *c = xe_create(&config);
    CHECK(c != NULL);
    if (!c)
        return 20;
    CHECK(c->timer->tr_node.io_Unit != NULL);
    CHECK(c->port->mp_Flags == PA_IGNORE);
    struct Library *TimerBase = (struct Library *)c->timer->tr_node.io_Device;
    for (unsigned i = 0; i < 20; ++i) {
        struct X86Registers r    = {.size = sizeof(r), .eip = i % 4 == 0 ? 2 : 0};
        volatile uint32_t cancel = i % 4 == 1;
        struct X86Limits l       = {sizeof(l), 1000000, 10000000, i % 4 == 2 ? 0 : 10, &cancel};
        struct X86Result result  = {.size = sizeof(result)};
        struct EClockVal start, end;
        if (i % 4 == 2)
            l.instructions = 10;
        CHECK(xe_set(c, &r) == 0);
        uint32_t hz     = ReadEClock(&start);
        uint32_t status = xe_run(c, &l, &result);
        ReadEClock(&end);
        CHECK(status == (i % 4 == 0   ? X86_STOP_HALT
                         : i % 4 == 1 ? X86_STOP_CANCEL
                         : i % 4 == 2 ? X86_STOP_LIMIT
                                      : X86_STOP_TIMEOUT));
        uint64_t ticks = (((uint64_t)end.ev_hi << 32) | end.ev_lo) - (((uint64_t)start.ev_hi << 32) | start.ev_lo);
        if (i % 4 == 3)
            CHECK(ticks * 1000 >= 10ULL * hz);
        CHECK(GetMsg(c->port) == NULL);
    }
    /* Timer interrupts cannot complete this request: clock fallback must. */
    struct X86Registers r   = {.size = sizeof(r)};
    struct X86Limits l      = {sizeof(l), 1000000, 10000000, 10, NULL};
    struct X86Result result = {.size = sizeof(result)};
    CHECK(xe_set(c, &r) == 0);
    Disable();
    uint32_t status = xe_run(c, &l, &result);
    Enable();
    CHECK(status == X86_STOP_TIMEOUT);
    CHECK(GetMsg(c->port) == NULL);
    CHECK(xe_delete(c) == 0);
    printf("DEADLINE RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
