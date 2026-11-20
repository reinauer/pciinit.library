/* PCIoprom --log: run folding and the file format. */
#include "private/accesslog.h"
#include <libraries/pciinit.h>
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x)                                   \
    do {                                           \
        ++checks;                                  \
        if (!(x)) {                                \
            ++failures;                            \
            printf("FAIL %u: %s\n", __LINE__, #x); \
        }                                          \
    } while (0)
/* Writes the log to a temporary file and reads it back. */
static int text(struct AccessLog *log, char *out, size_t size)
{
    FILE *f = tmpfile();
    size_t n;
    if (!f)
        return 0;
    if (accesslog_write(log, f)) {
        fclose(f);
        return 0;
    }
    rewind(f);
    n      = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    static char out[4096];
    struct AccessLog *log = accesslog_create();
    unsigned i;
    (void)argc;
    (void)argv;
    CHECK(log != NULL);
    if (!log)
        return 20;
    accesslog_mark(log, "POST");
    accesslog_add(log, PI_SPACE_IO, 0, 0x3c4, 1, 1, 0x01);
    accesslog_add(log, PI_SPACE_IO, 0, 0x3c5, 1, 0, 0x23);
    /* A polling loop: the same read repeated. */
    for (i = 0; i < 412; ++i)
        accesslog_add(log, PI_SPACE_IO, 0, 0x3da, 1, 0, 0x08);
    accesslog_add(log, PI_SPACE_IO, 0, 0x3da, 1, 0, 0x09);
    /* A fill: the same value at rising addresses. */
    for (i = 0; i < 0x4000; ++i)
        accesslog_add(log, PI_SPACE_LEGACY, 0, 0x18000 + 2 * i, 2, 1, 0x0720);
    /* Different values at rising addresses stay separate lines. */
    accesslog_add(log, PI_SPACE_LEGACY, 0, 0, 1, 1, 0x11);
    accesslog_add(log, PI_SPACE_LEGACY, 0, 1, 1, 1, 0x22);
    /* A run continues only at the next address. */
    accesslog_add(log, PI_SPACE_BAR, 2, 0x400, 4, 1, 0x7b23a040);
    accesslog_add(log, PI_SPACE_BAR, 2, 0x404, 4, 1, 0x7b23a040);
    accesslog_add(log, PI_SPACE_BAR, 2, 0x404, 4, 1, 0x7b23a040);
    accesslog_add(log, PI_SPACE_BAR, 0, 0x408, 4, 1, 0x7b23a040);
    accesslog_add(log, PI_SPACE_CONFIG, 0, 4, 2, 1, 0x0003);
    accesslog_mark(log, "SetVBEMode 0x111");
    accesslog_add(log, PI_SPACE_CONFIG, 0, 4, 2, 0, 0x0003);
    CHECK(text(log, out, sizeof(out)));
    CHECK(!strcmp(out,
                  "== POST\n"
                  "W IO   03c4 1 01\n"
                  "R IO   03c5 1 23\n"
                  "R IO   03da 1 08 rep 412\n"
                  "R IO   03da 1 09\n"
                  "W VGA  b8000 2 0720 seq 16384\n"
                  "W VGA  a0000 1 11\n"
                  "W VGA  a0001 1 22\n"
                  "W BAR2 00000400 4 7b23a040 seq 2\n"
                  "W BAR2 00000404 4 7b23a040\n"
                  "W BAR0 00000408 4 7b23a040\n"
                  "W CFG  04 2 0003\n"
                  "== SetVBEMode 0x111\n"
                  "R CFG  04 2 0003\n"
                  "; 16807 accesses in 14 lines\n"));
    if (failures)
        printf("%s", out);
    /* Writing empties the log. */
    CHECK(text(log, out, sizeof(out)) && !strcmp(out, "; 0 accesses in 0 lines\n"));
    accesslog_free(log);
    printf("ACCESSLOG RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
