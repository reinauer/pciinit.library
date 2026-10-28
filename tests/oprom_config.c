#include "private/oprom_config.h"
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
static void parse(struct OpromConfig *c, const char *text)
{
    oprom_config_parse(c, text, strlen(text));
}
#ifdef __mc68000__
__attribute__((stkparm))
#endif
int main(int argc, char **argv)
{
    struct OpromConfig c;
    char many[OPROM_CONFIG_MAX * 16 + 32] = "", line[16];
    unsigned i;
    FILE *f;
    (void)argc;
    (void)argv;
    parse(&c, "");
    CHECK(c.ignores == 0 && c.errors == 0 && !oprom_config_ignored(&c, 0x5333, 0x5631));
    parse(&c, "; comment only\n\n   \t\nIGNORE 5333:5631 ; S3 ViRGE\r\nignore 121A:*\nIgnore 1:2\n");
    CHECK(c.ignores == 3 && c.errors == 0);
    CHECK(oprom_config_ignored(&c, 0x5333, 0x5631) && !oprom_config_ignored(&c, 0x5333, 0x5632));
    CHECK(oprom_config_ignored(&c, 0x121a, 0x0001) && oprom_config_ignored(&c, 0x121a, 0xffff));
    CHECK(oprom_config_ignored(&c, 1, 2) && !oprom_config_ignored(&c, 2, 1));
    parse(&c,
          "Ignore 5333:5631\nReserve 0xC0000\nIgnore\nIgnore 5333:5631 extra\nIgnore 12345:1\nIgnore 1234:g\nIgnore "
          "1234\nIgnore 5333:*");
    CHECK(c.ignores == 2 && c.errors == 6);
    CHECK(!strcmp(c.error, "line 2: unknown command"));
    CHECK(oprom_config_ignored(&c, 0x5333, 0x1234));
    for (i = 0; i < OPROM_CONFIG_MAX + 1; ++i) {
        snprintf(line, sizeof(line), "Ignore %x:1\n", i + 1);
        strcat(many, line);
    }
    parse(&c, many);
    CHECK(c.ignores == OPROM_CONFIG_MAX && c.errors == 1 && strstr(c.error, "too many"));
    CHECK(oprom_config_load(&c, "this-file-does-not-exist") == NULL && c.ignores == 0);
    f = fopen("oprom-config-test.tmp", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("Ignore 10de:*\n", f);
        fclose(f);
        CHECK(oprom_config_load(&c, "oprom-config-test.tmp") != NULL);
        CHECK(c.ignores == 1 && c.errors == 0 && oprom_config_ignored(&c, 0x10de, 0x28));
        remove("oprom-config-test.tmp");
    }
    printf("OPROM CONFIG RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 20 : 0;
}
