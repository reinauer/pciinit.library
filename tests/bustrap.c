/* Native check of the bus-error trap handler in src/pciinit/bus.S. A 68k
 * bus error cannot be provoked on demand, so tests/bustrap_s.S enters the
 * handler as exec would, with a hand-built frame of each format the
 * handler accepts and of cases it must pass on. Runs on the 68020-68060;
 * the frames do not depend on the CPU running the test. */
#include "private/bus.h"
#include <proto/exec.h>
#include <exec/execbase.h>
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

void bt_entry(void);
void bt_old_handler(void);
void bt_rts(void);
extern uint32_t bt_size, bt_pc, bt_fmtvec, bt_addr_off, bt_addr, bt_trap;
extern uint32_t bt_regs[4], bt_sp_before, bt_sp_after, bt_seen_trap;

static struct PIBusTrap trap;

/* One handler entry; returns with the task's trap fields as they were. */
static void enter(uint32_t number, uint32_t vector, unsigned format, uint32_t size, uint32_t offset, uint32_t pc,
                  uint32_t address)
{
    struct Task *task = FindTask(NULL);
    APTR code = task->tc_TrapCode, data = task->tc_TrapData;
    trap         = (struct PIBusTrap){(APTR)bt_old_handler, (APTR)0x5a5a5a5a, 0, 0};
    bt_trap      = number;
    bt_size      = size;
    bt_addr_off  = offset;
    bt_pc        = pc;
    bt_fmtvec    = (format << 12) | (vector * 4);
    bt_addr      = address;
    bt_seen_trap = 0;
    memset(bt_regs, 0, sizeof(bt_regs));
    Disable();
    task->tc_TrapData = &trap;
    task->tc_TrapCode = (APTR)pi_bus_trap;
    Supervisor((ULONG (*)())(APTR)bt_entry);
    /* Record what the handler left before putting the task back. */
    trap.code         = task->tc_TrapCode;
    trap.data         = task->tc_TrapData;
    task->tc_TrapCode = code;
    task->tc_TrapData = data;
    Enable();
}

/* number is the trap exec hands over: 2, or 4 for the bus error frame its
 * vector 2 code passes on through the illegal instruction vector. */
static void recovered_case(uint32_t number, unsigned format, uint32_t size, uint32_t offset)
{
    uint32_t pc = (uint32_t)pi_bus_read32 + 2;
    enter(number, 2, format, size, offset, pc, 0x40001230);
    CHECK(bt_regs[0] == 0xffffffffU);
    CHECK(bt_regs[1] == 0x22222222 && bt_regs[2] == 0x33333333 && bt_regs[3] == 0x44444444);
    CHECK(bt_sp_after == bt_sp_before);
    CHECK(trap.faults == 1 && trap.address == 0x40001230);
    CHECK(bt_seen_trap == 0);
    /* Still installed for the next access. */
    CHECK(trap.code == (APTR)pi_bus_trap && trap.data == &trap);
    printf("trap %lu format $%X: d0=%08lx faults=%lu address=%08lx\n", (unsigned long)number, format,
           (unsigned long)bt_regs[0], (unsigned long)trap.faults, (unsigned long)trap.address);
}

/* The handler gives the task its own handler and data back and passes
 * the trap on with the registers and the stack untouched. */
static void passed_case(uint32_t number, uint32_t pc, const char *what)
{
    enter(number, number, 0, 8, 2, pc, 0);
    CHECK(bt_seen_trap == number);
    CHECK(bt_regs[0] == 0x11111111 && bt_regs[1] == 0x22222222 && bt_regs[2] == 0x33333333 && bt_regs[3] == 0x44444444);
    CHECK(bt_sp_after == bt_sp_before);
    CHECK(trap.code == (APTR)bt_old_handler && trap.data == (APTR)0x5a5a5a5a);
    CHECK(trap.faults == 0);
    printf("passed on: %s\n", what);
}

/* Real accesses through the routines with no fault in sight. */
static void routine_case(void)
{
    static uint32_t ram[4];
    static const uint8_t source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pi_bus_write32(ram, 0x01020304);
    CHECK(pi_bus_read32(ram) == 0x01020304);
    pi_bus_write16((uint8_t *)ram + 4, 0xa1b2);
    CHECK(pi_bus_read16((uint8_t *)ram + 4) == 0xa1b2);
    pi_bus_write8((uint8_t *)ram + 6, 0x7f);
    CHECK(pi_bus_read8((uint8_t *)ram + 6) == 0x7f);
    pi_bus_fill16(ram, 0x5555, 4);
    CHECK(ram[0] == 0x55555555 && ram[1] == 0x55555555);
    pi_bus_copy32(ram, source, 2);
    CHECK(!memcmp(ram, source, 8));
    pi_bus_fill8(ram, 0xee, 3);
    CHECK(((uint8_t *)ram)[2] == 0xee && ((uint8_t *)ram)[3] == 4);
}

int main(void)
{
    printf("CPU attn flags %04x\n", SysBase->AttnFlags);
    routine_case();
    recovered_case(2, 0xa, 32, 16);
    recovered_case(2, 0xb, 92, 16);
    recovered_case(2, 0x7, 60, 20);
    recovered_case(2, 0x4, 16, 8);
    recovered_case(4, 0x7, 60, 20);
    recovered_case(4, 0x4, 16, 8);
    passed_case(5, (uint32_t)pi_bus_read32 + 2, "trap 5 inside the window");
    passed_case(4, (uint32_t)pi_bus_read32 + 2, "illegal instruction inside the window");
    passed_case(2, (uint32_t)bt_rts, "access fault outside the window");
    passed_case(2, (uint32_t)pi_bus_read32 + 2, "access fault with an unknown frame format");
    printf("BUSTRAP RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
