#ifndef PCIINIT_ROM_H
#define PCIINIT_ROM_H
#include <libraries/pciinit.h>
uint32_t pi_rom_parse(const uint8_t *data, uint32_t bytes, struct PCIInitROMInfo *info);
#endif
