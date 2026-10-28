#ifndef PCIINIT_QUIRKS_H
#define PCIINIT_QUIRKS_H
#include <stdint.h>
/* Known option ROM images, identified by the CRC-32 (IEEE 802.3) of their
 * initialization image, receive small patches in the RAM copy before
 * execution. The card ROM itself is never written. Every entry documents
 * what the patched bytes mean; nothing here changes emulator policy. */
struct PIPatch
{
    uint32_t offset;
    uint32_t width;
    uint32_t value;
}; /* width 0 ends a list */
struct PIQuirk
{
    uint32_t crc, bytes;
    const struct PIPatch *patches;
    const char *note;
};
uint32_t pi_crc32(const uint8_t *data, uint32_t bytes);
const struct PIQuirk *pi_find_quirk(uint32_t crc, uint32_t bytes);
/* Returns the number of patches applied to the image copy. */
uint32_t pi_apply_patches(uint8_t *image, uint32_t bytes, const struct PIPatch *patches);
#endif
