#ifndef PCIINIT_ACCESSLOG_H
#define PCIINIT_ACCESSLOG_H
#include <stdint.h>
#include <stdio.h>
/* PCIoprom --log: the card accesses an access hook sees, in order, with
 * runs folded together. A run is the same access repeated at one address
 * (a polling loop) or the same value at rising addresses one width apart
 * (a fill). Entries live in memory until accesslog_write(), because DOS
 * must not be called while a ROM runs. */
struct AccessLog;
#define ACCESSLOG_MAX_ENTRIES (1024UL * 1024UL) /* 16 MiB; later accesses are counted, not kept */
struct AccessLog *accesslog_create(void);
void accesslog_free(struct AccessLog *);
/* A section heading such as "POST" or "SetVBEMode 0x111"; copied. */
void accesslog_mark(struct AccessLog *, const char *label);
/* One access as the hook reports it: space PI_SPACE_*, bar for
 * PI_SPACE_BAR, offset, width in bytes, write flag and the value written
 * or read. */
void accesslog_add(struct AccessLog *, uint32_t space, uint32_t bar, uint32_t offset, uint32_t width, uint32_t write,
                   uint32_t value);
/* Writes the kept entries and empties the log; returns 0 on success. */
int accesslog_write(struct AccessLog *, FILE *);
/* The column legend, once at the top of a file. */
int accesslog_legend(FILE *);
#endif
