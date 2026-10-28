#include "private/oprom_config.h"
#include <stdio.h>
#include <string.h>

static int hex_field(const char *s, size_t n, unsigned *value)
{
    unsigned v = 0;
    size_t i;
    if (!n || n > 4)
        return 0;
    for (i = 0; i < n; ++i) {
        char ch = s[i];
        unsigned d;
        if (ch >= '0' && ch <= '9')
            d = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            d = ch - 'A' + 10;
        else
            return 0;
        v = v * 16 + d;
    }
    *value = v;
    return 1;
}
static int word_equal(const char *s, size_t n, const char *word)
{
    size_t i;
    if (strlen(word) != n)
        return 0;
    for (i = 0; i < n; ++i) {
        char a = s[i], b = word[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}
static void reject(struct OpromConfig *c, unsigned line, const char *why)
{
    if (!c->errors++)
        snprintf(c->error, sizeof(c->error), "line %u: %s", line, why);
}
/* Returns the next word in [*p,end) or NULL. */
static const char *word(const char **p, const char *end, size_t *n)
{
    const char *s = *p, *w;
    while (s < end && (*s == ' ' || *s == '\t'))
        ++s;
    w = s;
    while (s < end && *s != ' ' && *s != '\t')
        ++s;
    *p = s;
    *n = (size_t)(s - w);
    return *n ? w : NULL;
}
void oprom_config_parse(struct OpromConfig *c, const char *text, size_t length)
{
    const char *p = text, *end = text + length;
    unsigned line = 0;
    memset(c, 0, sizeof(*c));
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p)), *stop, *cmd, *arg, *colon, *extra;
        size_t n, an, xn;
        unsigned vendor, device = 0;
        int any = 0;
        if (!eol)
            eol = end;
        ++line;
        stop = memchr(p, ';', (size_t)(eol - p));
        if (!stop)
            stop = eol;
        if (stop > p && stop[-1] == '\r')
            --stop;
        cmd = word(&p, stop, &n);
        if (cmd) {
            if (!word_equal(cmd, n, "ignore"))
                reject(c, line, "unknown command");
            else if (!(arg = word(&p, stop, &an)))
                reject(c, line, "Ignore needs vendor:device");
            else if ((extra = word(&p, stop, &xn)) != NULL)
                reject(c, line, "unexpected text after vendor:device");
            else if (!(colon = memchr(arg, ':', an)) || !hex_field(arg, (size_t)(colon - arg), &vendor))
                reject(c, line, "vendor must be 1-4 hex digits before ':'");
            else if (!((any = (an - (size_t)(colon + 1 - arg) == 1 && colon[1] == '*')) ||
                       hex_field(colon + 1, an - (size_t)(colon + 1 - arg), &device)))
                reject(c, line, "device must be 1-4 hex digits or *");
            else if (c->ignores == OPROM_CONFIG_MAX)
                reject(c, line, "too many Ignore entries");
            else {
                struct OpromIgnore *i = &c->ignore[c->ignores++];
                i->vendor             = vendor;
                i->device             = device;
                i->any_device         = any;
            }
        }
        p = eol < end ? eol + 1 : end;
    }
}
int oprom_config_ignored(const struct OpromConfig *c, unsigned vendor, unsigned device)
{
    unsigned i;
    for (i = 0; i < c->ignores; ++i)
        if (c->ignore[i].vendor == vendor && (c->ignore[i].any_device || c->ignore[i].device == device))
            return 1;
    return 0;
}
const char *oprom_config_load(struct OpromConfig *c, const char *override)
{
    static const char *const paths[] = {"ENV:" OPROM_CONFIG_NAME, "ENVARC:" OPROM_CONFIG_NAME};
    static char text[16384];
    unsigned i;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < 2; ++i) {
        const char *path = override ? override : paths[i];
        FILE *f          = fopen(path, "rb");
        size_t n;
        if (!f) {
            if (override)
                return NULL;
            continue;
        }
        n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        oprom_config_parse(c, text, n);
        if (n == sizeof(text) - 1)
            reject(c, 0, "file longer than 16 KiB; rest ignored");
        return path;
    }
    return NULL;
}
