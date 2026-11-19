#ifndef PCIINIT_BUS_H
#define PCIINIT_BUS_H
#include <stdint.h>
#include <exec/types.h>

/* Card loads and stores that survive a 68k bus error (src/pciinit/bus.S).
 * While pi_hardware_start() has installed pi_bus_trap, a bus error in these
 * routines ends it as an aborted PCI cycle would: a read returns all-ones,
 * a store is dropped and a fill or copy stops early. The trap handler
 * counts the event in the task's PIBusTrap. Values are raw 68k bus order;
 * callers swap. The routines use only D0, D1, A0 and A1 and no stack. */
struct PIBusTrap
{
    APTR code, data;          /* the task's own tc_TrapCode and tc_TrapData */
    uint32_t faults, address; /* bus errors caught, Amiga address of the first */
};
#define PI_BUS_REG(type, name, reg) type name __asm(#reg)
uint32_t pi_bus_read8(PI_BUS_REG(volatile void *, address, a0));
uint32_t pi_bus_read16(PI_BUS_REG(volatile void *, address, a0));
uint32_t pi_bus_read32(PI_BUS_REG(volatile void *, address, a0));
void pi_bus_write8(PI_BUS_REG(volatile void *, address, a0), PI_BUS_REG(uint32_t, value, d0));
void pi_bus_write16(PI_BUS_REG(volatile void *, address, a0), PI_BUS_REG(uint32_t, value, d0));
void pi_bus_write32(PI_BUS_REG(volatile void *, address, a0), PI_BUS_REG(uint32_t, value, d0));
/* count elements, at least one */
void pi_bus_fill8(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(uint32_t, value, d0),
                  PI_BUS_REG(uint32_t, count, d1));
void pi_bus_fill16(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(uint32_t, value, d0),
                   PI_BUS_REG(uint32_t, count, d1));
void pi_bus_fill32(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(uint32_t, value, d0),
                   PI_BUS_REG(uint32_t, count, d1));
void pi_bus_copy8(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(const void *, source, a1),
                  PI_BUS_REG(uint32_t, count, d0));
void pi_bus_copy16(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(const void *, source, a1),
                   PI_BUS_REG(uint32_t, count, d0));
void pi_bus_copy32(PI_BUS_REG(volatile void *, destination, a0), PI_BUS_REG(const void *, source, a1),
                   PI_BUS_REG(uint32_t, count, d0));
/* Exec's tc_TrapCode entry; see bus.S. */
void pi_bus_trap(void);
#endif
