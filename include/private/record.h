#ifndef PCIINIT_RECORD_H
#define PCIINIT_RECORD_H
#include "libraries/x86emu.h"
/* Private test stream, native big-endian words. Never a library ABI. */
#define PI_RECORD_MAGIC 0x50495231U
enum PIRecordKind
{
    PI_RECORD_RAM = 1,
    PI_RECORD_REGS,
    PI_RECORD_ACCESS,
    PI_RECORD_BULK,
    PI_RECORD_INTERRUPT,
    PI_RECORD_FINAL_RAM,
    PI_RECORD_FINAL_REGS,
    PI_RECORD_RESULT
};
struct PIRecordAccess
{
    uint32_t address, width, operation, value, status;
};
struct PIRecordInterrupt
{
    uint32_t number, fault, handled;
    struct X86Registers before, after;
};
#ifdef PCIINIT_RECORD
void pi_record(uint32_t kind, const void *data, uint32_t bytes);
void pi_record_mark(unsigned phase);
void pi_profile_begin(unsigned category);
void pi_profile_end(unsigned category);
#endif
#endif
