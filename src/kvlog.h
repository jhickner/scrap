#ifndef KVLOG_H
#define KVLOG_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVLOG_MAPS 4
#define KVLOG_CAP  4096

struct kvlog_ent {
    char *key;
    char *val;
};

struct kvlog_map {
    char             path[4096];
    struct kvlog_ent ents[KVLOG_CAP];
    int              n;
    int              live;
    int              loaded;
    time_t           mtime;
    off_t            size;
};

static struct kvlog_map kvlog_maps[KVLOG_MAPS];
static int              kvlog_clock;

static inline void kvlog_clear(struct kvlog_map *m)
{
    for (int i = 0; i < m->n; i++) {
        free(m->ents[i].key);
        free(m->ents[i].val);
        m->ents[i].key = m->ents[i].val = NULL;
    }
    m->n = 0;
    m->loaded = 0;
    m->mtime = 0;
    m->size = 0;
}

static inline int kvlog_upsert(struct kvlog_map *m, const char *key,
                               const char *val)
{
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->ents[i].key, key) != 0)
            continue;
        char *copy = strdup(val);
        if (!copy)
            return 0;
        free(m->ents[i].val);
        struct kvlog_ent ent = {m->ents[i].key, copy};
        memmove(&m->ents[i], &m->ents[i + 1],
                (size_t)(m->n - i - 1) * sizeof *m->ents);
        m->ents[m->n - 1] = ent;
        return 1;
    }
    char *k = strdup(key);
    char *v = strdup(val);
    if (!k || !v) {
        free(k);
        free(v);
        return 0;
    }
    if (m->n >= KVLOG_CAP) {
        free(m->ents[0].key);
        free(m->ents[0].val);
        memmove(&m->ents[0], &m->ents[1], (size_t)(m->n - 1) * sizeof *m->ents);
        m->n--;
    }
    m->ents[m->n].key = k;
    m->ents[m->n].val = v;
    m->n++;
    return 1;
}

static inline struct kvlog_map *kvlog_slot(const char *path)
{
    for (int i = 0; i < KVLOG_MAPS; i++)
        if (kvlog_maps[i].live && strcmp(kvlog_maps[i].path, path) == 0)
            return &kvlog_maps[i];

    int at = -1;
    for (int i = 0; i < KVLOG_MAPS; i++)
        if (!kvlog_maps[i].live) {
            at = i;
            break;
        }
    if (at < 0) {
        at = kvlog_clock++ % KVLOG_MAPS;
        kvlog_clear(&kvlog_maps[at]);
    }

    struct kvlog_map *m = &kvlog_maps[at];
    snprintf(m->path, sizeof m->path, "%s", path);
    m->n = 0;
    m->live = 1;
    m->loaded = 0;
    m->mtime = 0;
    return m;
}

static inline void kvlog_load(struct kvlog_map *m)
{
    kvlog_clear(m);
    struct stat st;
    if (stat(m->path, &st) == 0) {
        m->mtime = st.st_mtime;
        m->size = st.st_size;
    }

    FILE *f = fopen(m->path, "r");
    if (!f) {
        m->loaded = 1;
        return;
    }

    char  *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        char *value = tab + 1;
        value[strcspn(value, "\n")] = '\0';
        if (*line && *value)
            kvlog_upsert(m, line, value);
    }
    free(line);
    fclose(f);
    m->loaded = 1;
}

static inline struct kvlog_map *kvlog_fresh(const char *path)
{
    struct kvlog_map *m = kvlog_slot(path);
    struct stat       st;
    time_t            mt = 0;
    off_t             size = 0;
    if (stat(path, &st) == 0) {
        mt = st.st_mtime;
        size = st.st_size;
    }
    if (!m->loaded || mt != m->mtime || size != m->size)
        kvlog_load(m);
    return m;
}

static inline int kvlog_lookup(const char *path, const char *key, char *out,
                               size_t size)
{
    struct kvlog_map *m = kvlog_fresh(path);
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->ents[i].key, key) != 0)
            continue;
        snprintf(out, size, "%s", m->ents[i].val);
        return 1;
    }
    return 0;
}

static inline int kvlog_append(const char *path, const char *key,
                               const char *value)
{
    struct kvlog_map *m = kvlog_fresh(path);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return 0;

    size_t size = strlen(key) + strlen(value) + 3;
    char  *row = malloc(size);
    if (!row) {
        close(fd);
        return 0;
    }
    int n = snprintf(row, size, "%s\t%s\n", key, value);
    size_t left = n > 0 && (size_t)n < size ? (size_t)n : 0;
    const char *p = row;
    while (left) {
        ssize_t written = write(fd, p, left);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (written == 0)
            break;
        p += written;
        left -= (size_t)written;
    }
    free(row);
    close(fd);
    if (left != 0 || n <= 0)
        return 0;

    if (value && *value && !kvlog_upsert(m, key, value)) {
        m->loaded = 0;
        return 1;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        m->mtime = st.st_mtime;
        m->size = st.st_size;
    }
    return 1;
}

#endif
