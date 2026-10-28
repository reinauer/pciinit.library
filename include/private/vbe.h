#ifndef PCIINIT_VBE_PRIVATE_H
#define PCIINIT_VBE_PRIVATE_H
#include <libraries/pciinit.h>
#define PI_VBE_BUFFER 0x8000UL
#define PI_VBE_ENTRY  0x7200UL
uint32_t pi_vbe_parse_info(const uint8_t *, struct PCIInitVBEInfo *);
uint32_t pi_vbe_parse_mode(const uint8_t *, uint32_t, uint32_t, struct PCIInitVBEModeInfo *);
uint32_t pi_vbe_layout(const struct PCIInitVBEModeInfo *, uint32_t);
#endif
