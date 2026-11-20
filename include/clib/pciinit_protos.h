#ifndef AMIGA_CLIB_PCIINIT_PROTOS_H
#define AMIGA_CLIB_PCIINIT_PROTOS_H
#include <libraries/pciinit.h>
struct TagItem;
PCIInitImage *LoadOptionROMFromFile(const char *, const struct TagItem *);
PCIInitImage *LoadOptionROMFromCard(struct pci_dev *, const struct TagItem *);
uint32_t FreeOptionROM(PCIInitImage *);
uint32_t GetOptionROMInfo(const PCIInitImage *, struct PCIInitROMInfo *);
PCIInitContext *CreatePCIContext(struct pci_dev *, const struct TagItem *);
uint32_t DeletePCIContext(PCIInitContext *);
uint32_t RunOptionROM(PCIInitContext *, const PCIInitImage *);
uint32_t GetPCIContextInfo(const PCIInitContext *, struct PCIInitContextInfo *);
uint32_t GetVBEInfo(PCIInitContext *, struct PCIInitVBEInfo *);
uint32_t GetVBEModeInfo(PCIInitContext *, uint32_t, struct PCIInitVBEModeInfo *);
uint32_t SetVBEMode(PCIInitContext *, uint32_t, uint32_t);
uint32_t GetCurrentVBEMode(PCIInitContext *, uint32_t *);
uint32_t MapFramebuffer(PCIInitContext *, struct PCIInitFramebuffer *);
uint32_t UnmapFramebuffer(PCIInitContext *);
uint32_t WriteFramebuffer(PCIInitContext *, uint32_t, const void *, uint32_t);
uint32_t ReadFramebuffer(PCIInitContext *, uint32_t, void *, uint32_t);
uint32_t ObtainLegacyVGA(PCIInitContext *);
uint32_t ReleaseLegacyVGA(PCIInitContext *);
#endif
