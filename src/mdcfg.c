#include "mdcfg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"

#define FENCE "---"

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int mdcfg_load(struct mdcfg *m, const char *path)
{
    char *text = text_slurp(path, 1u << 20, NULL);
    if (!text) {
        memset(m, 0, sizeof *m);
        return 0;
    }
    return mdcfg_parse(m, text);
}

int mdcfg_parse(struct mdcfg *m, char *text)
{
    memset(m, 0, sizeof *m);
    if (!text)
        return 0;
    m->text = text;

    char *p = m->text;

    if (strncmp(p, FENCE, 3) || (p[3] != '\n' && p[3] != '\r')) {
        m->body = m->text;
        return 1;
    }

    p = strchr(p, '\n');
    p = p ? p + 1 : m->text + strlen(m->text);

    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        char *line = trim(p);
        if (!strcmp(line, FENCE)) {
            p = nl ? nl + 1 : p + strlen(p);
            break;
        }
        char *colon = strchr(line, ':');
        if (colon && m->n < MDCFG_KEYS) {
            *colon = '\0';
            m->keys[m->n] = trim(line);
            m->values[m->n] = trim(colon + 1);
            m->n++;
        }
        if (!nl) {
            p += strlen(p);
            break;
        }
        p = nl + 1;
    }

    while (*p == '\n' || *p == '\r')
        p++;
    m->body = p;
    return 1;
}

void mdcfg_free(struct mdcfg *m)
{
    free(m->text);
    memset(m, 0, sizeof *m);
}

const char *mdcfg_get(const struct mdcfg *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->keys[i], key))
            return m->values[i];
    return "";
}

int mdcfg_int(const struct mdcfg *m, const char *key, int fallback)
{
    const char *s = mdcfg_get(m, key);
    if (!*s)
        return fallback;
    char *end = NULL;
    long  v = strtol(s, &end, 10);
    return end && end != s ? (int)v : fallback;
}

struct cfg_out {
    const char *const *keys;
    const char *const *values;
    int                n;
    const char        *body;
};

static int write_cfg(FILE *f, void *ud)
{
    const struct cfg_out *c = ud;

    int ok = fprintf(f, "%s\n", FENCE) > 0;
    for (int i = 0; i < c->n && ok; i++)
        ok = fprintf(f, "%s: %s\n", c->keys[i], c->values[i] ? c->values[i] : "") > 0;
    if (ok)
        ok = fprintf(f, "%s\n", FENCE) > 0;
    if (ok && c->body && *c->body) {
        size_t len = strlen(c->body);
        ok = fprintf(f, "\n%s%s", c->body, c->body[len - 1] == '\n' ? "" : "\n") > 0;
    }
    return ok;
}

int mdcfg_write(const char *path, const char *const *keys,
                const char *const *values, int n, const char *body)
{
    struct cfg_out out = {keys, values, n, body};
    return text_spit(path, write_cfg, &out);
}

int mdcfg_list(const char *dir, char names[][MDCFG_NAME], int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int            n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 3, ".md"))
            continue;
        if (len - 3 >= MDCFG_NAME)
            continue;

        char name[MDCFG_NAME];
        snprintf(name, sizeof name, "%.*s", (int)(len - 3), e->d_name);

        int at = n++;
        while (at > 0 && strcmp(names[at - 1], name) > 0) {
            memcpy(names[at], names[at - 1], MDCFG_NAME);
            at--;
        }
        memcpy(names[at], name, MDCFG_NAME);
    }
    closedir(d);
    return n;
}

int mdcfg_dir(char *out, size_t size, const char *leaf)
{
    return path_config_subdir(out, size, leaf);
}
