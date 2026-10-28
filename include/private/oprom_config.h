#ifndef PCIINIT_OPROM_CONFIG_H
#define PCIINIT_OPROM_CONFIG_H
#include <stddef.h>
#include <stdint.h>
/* PCIoprom-Configuration: one command per line, ';' starts a comment.
 *   Ignore <vendor>:<device>   never run this card's ROM; <device> may be *
 * Read from ENV: first, then ENVARC:. */
#define OPROM_CONFIG_NAME "PCIoprom-Configuration"
#define OPROM_CONFIG_MAX  32
struct OpromIgnore
{
    uint16_t vendor, device;
    uint8_t any_device;
};
struct OpromConfig
{
    unsigned ignores, errors;
    struct OpromIgnore ignore[OPROM_CONFIG_MAX];
    char error[96]; /* the first rejected line, for the user */
};
/* Parse file contents; rejected lines are counted and skipped. */
void oprom_config_parse(struct OpromConfig *c, const char *text, size_t length);
int oprom_config_ignored(const struct OpromConfig *c, unsigned vendor, unsigned device);
/* Read the override path, else ENV: then ENVARC:. Returns the path read,
 * or NULL when no file exists; the configuration is then empty. */
const char *oprom_config_load(struct OpromConfig *c, const char *override);
#endif
