#include "private/accesslog.h"
#include <libraries/pciinit.h>
#include <stdlib.h>
#include <string.h>
#define CHUNK_ENTRIES 4096U
#define SPACE_MARK    0xffU
/* One access or run. stride is 0 for repeats at one address, the width
 * for a run at rising addresses; a mark keeps its label index in offset. */
struct Entry
{
    uint32_t offset, value, count;
    uint8_t space, bar, width, flags;
};
#define FLAG_WRITE  1U
#define FLAG_STRIDE 2U
struct Chunk
{
    struct Chunk *next;
    unsigned used;
    struct Entry entries[CHUNK_ENTRIES];
};
struct AccessLog
{
    struct Chunk *first, *last;
    unsigned long kept, dropped, accesses;
    char **labels;
    unsigned label_count;
};
struct AccessLog *accesslog_create(void)
{
    return calloc(1, sizeof(struct AccessLog));
}
static void clear(struct AccessLog *log)
{
    struct Chunk *c = log->first, *next;
    unsigned i;
    for (; c; c = next) {
        next = c->next;
        free(c);
    }
    for (i = 0; i < log->label_count; ++i)
        free(log->labels[i]);
    free(log->labels);
    log->first = log->last = NULL;
    log->labels            = NULL;
    log->label_count       = 0;
    log->kept = log->dropped = log->accesses = 0;
}
void accesslog_free(struct AccessLog *log)
{
    if (log) {
        clear(log);
        free(log);
    }
}
static struct Entry *latest(struct AccessLog *log)
{
    return log->last && log->last->used ? &log->last->entries[log->last->used - 1] : NULL;
}
static struct Entry *append(struct AccessLog *log)
{
    struct Chunk *c = log->last;
    if (log->kept >= ACCESSLOG_MAX_ENTRIES)
        return NULL;
    if (!c || c->used == CHUNK_ENTRIES) {
        c = malloc(sizeof(*c));
        if (!c)
            return NULL;
        c->next = NULL;
        c->used = 0;
        if (log->last)
            log->last->next = c;
        else
            log->first = c;
        log->last = c;
    }
    ++log->kept;
    return &c->entries[c->used++];
}
void accesslog_mark(struct AccessLog *log, const char *label)
{
    char **labels = realloc(log->labels, (log->label_count + 1) * sizeof(*labels));
    char *copy    = malloc(strlen(label) + 1);
    struct Entry *e;
    if (labels)
        log->labels = labels;
    if (!labels || !copy || !(e = append(log))) {
        free(copy);
        return;
    }
    strcpy(copy, label);
    log->labels[log->label_count] = copy;
    *e                            = (struct Entry){log->label_count++, 0, 0, SPACE_MARK, 0, 0, 0};
}
void accesslog_add(struct AccessLog *log, uint32_t space, uint32_t bar, uint32_t offset, uint32_t width, uint32_t write,
                   uint32_t value)
{
    struct Entry *e = latest(log);
    unsigned flags  = write ? FLAG_WRITE : 0;
    ++log->accesses;
    if (e && e->space == space && e->bar == bar && e->width == width && (e->flags & FLAG_WRITE) == flags &&
        e->value == value) {
        if (e->count == 1 && offset == e->offset + width) {
            e->flags |= FLAG_STRIDE;
            e->count = 2;
            return;
        }
        if (!(e->flags & FLAG_STRIDE) ? offset == e->offset : offset == e->offset + e->count * width) {
            ++e->count;
            return;
        }
    }
    e = append(log);
    if (!e) {
        ++log->dropped;
        return;
    }
    *e = (struct Entry){offset, value, 1, space, bar, width, flags};
}
int accesslog_legend(FILE *f)
{
    return fputs(
               "; R/W space address width value [rep N | seq N]\n"
               ";   space: IO port, VGA memory address (A0000-BFFFF), BARn offset, CFG register\n"
               ";   value: the number as the x86 sees it; reads give what the card returned\n"
               ";   rep N: the same access N times; seq N: N accesses at rising addresses, width apart\n",
               f) < 0;
}
static int line(FILE *f, const struct AccessLog *log, const struct Entry *e)
{
    char space[8];
    unsigned long address = e->offset;
    int digits            = 8, status;
    if (e->space == SPACE_MARK)
        return fprintf(f, "== %s\n", log->labels[e->offset]) < 0;
    switch (e->space) {
    case PI_SPACE_IO:
        strcpy(space, "IO");
        digits = 4;
        break;
    case PI_SPACE_LEGACY:
        strcpy(space, "VGA");
        address += 0xa0000UL;
        digits = 5;
        break;
    case PI_SPACE_BAR:
        snprintf(space, sizeof(space), "BAR%u", (unsigned)e->bar);
        break;
    default:
        strcpy(space, "CFG");
        digits = 2;
        break;
    }
    status = fprintf(f, "%c %-4s %0*lx %u %0*lx", e->flags & FLAG_WRITE ? 'W' : 'R', space, digits, address,
                     (unsigned)e->width, 2 * e->width, (unsigned long)e->value);
    if (status >= 0 && e->count > 1)
        status = fprintf(f, " %s %lu", e->flags & FLAG_STRIDE ? "seq" : "rep", (unsigned long)e->count);
    return status < 0 || fputc('\n', f) == EOF;
}
int accesslog_write(struct AccessLog *log, FILE *f)
{
    const struct Chunk *c;
    unsigned i;
    int failed = 0;
    for (c = log->first; c && !failed; c = c->next)
        for (i = 0; i < c->used && !failed; ++i)
            failed = line(f, log, &c->entries[i]);
    if (!failed && log->dropped)
        failed = fprintf(f, "; log full: %lu later accesses not kept\n", log->dropped) < 0;
    if (!failed)
        failed = fprintf(f, "; %lu accesses in %lu lines\n", log->accesses, log->kept) < 0;
    clear(log);
    return failed;
}
