#include "mdcfg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    memset(m, 0, sizeof *m);
    m->text = text_slurp(path, 1u << 20, NULL);
    if (!m->text)
        return 0;

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

int mdcfg_write(const char *path, const char *const *keys,
                const char *const *values, int n, const char *body)
{
    char tmp[4300];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
        return 0;

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;

    int ok = fprintf(f, "%s\n", FENCE) > 0;
    for (int i = 0; i < n && ok; i++)
        ok = fprintf(f, "%s: %s\n", keys[i], values[i] ? values[i] : "") > 0;
    if (ok)
        ok = fprintf(f, "%s\n", FENCE) > 0;
    if (ok && body && *body) {
        size_t len = strlen(body);
        ok = fprintf(f, "\n%s%s", body, body[len - 1] == '\n' ? "" : "\n") > 0;
    }
    if (fclose(f) != 0)
        ok = 0;

    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
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
    char base[4096];
    if (!path_config_dir(base, sizeof base))
        return 0;
    if ((size_t)snprintf(out, size, "%s/%s", base, leaf) >= size)
        return 0;

    for (char *p = out + strlen(base) + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(out, 0700);
        *p = '/';
    }
    mkdir(out, 0700);
    return 1;
}
