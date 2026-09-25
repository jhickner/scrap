#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filelock.h"
#include "text.h"

static struct settings global;

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int find(const struct settings *s, const char *key)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->entries[i].key, key) == 0)
            return i;
    return -1;
}

void settings_load(struct settings *s, const char *path)
{
    s->count = 0;
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");
    if (!s->path[0])
        return;

    FILE *f = fopen(s->path, "r");
    if (!f)
        return;

    char line[512];
    while (s->count < MAX_SETTINGS && fgets(line, sizeof line, f)) {
        char *t = trim(line);
        if (!*t || *t == '#')
            continue;
        char *eq = strchr(t, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(t);
        char *value = trim(eq + 1);
        if (!*key || strlen(key) >= MAX_SETTING_KEY)
            continue;
        snprintf(s->entries[s->count].key, MAX_SETTING_KEY, "%s", key);
        snprintf(s->entries[s->count].value, MAX_SETTING_VALUE, "%s", value);
        s->count++;
    }
    fclose(f);
}

const char *settings_get(const struct settings *s, const char *key,
                         const char *fallback)
{
    int i = find(s, key);
    return i < 0 ? fallback : s->entries[i].value;
}

static int write_entries(FILE *f, void *ud)
{
    struct settings *s = ud;
    for (int j = 0; j < s->count; j++)
        fprintf(f, "%s=%s\n", s->entries[j].key, s->entries[j].value);
    return 1;
}

static void put(struct settings *s, const char *key, const char *value)
{
    int i = find(s, key);
    if (i < 0) {
        if (s->count >= MAX_SETTINGS || strlen(key) >= MAX_SETTING_KEY)
            return;
        i = s->count++;
        snprintf(s->entries[i].key, MAX_SETTING_KEY, "%s", key);
    }
    snprintf(s->entries[i].value, MAX_SETTING_VALUE, "%s", value);
}

void settings_put(struct settings *s, const char *key, const char *value)
{
    if (!value)
        return;
    if (!s->path[0]) {
        put(s, key, value);
        return;
    }
    int i = find(s, key);
    if (i >= 0 && strcmp(s->entries[i].value, value) == 0)
        return;

    int lock = filelock_acquire(s->path, LOCK_EX);
    struct settings *fresh = malloc(sizeof *fresh);
    if (fresh) {
        settings_load(fresh, s->path);
        *s = *fresh;
        free(fresh);
    }
    put(s, key, value);
    text_spit(s->path, write_entries, s);
    filelock_release(lock);
}

void settings_open(const char *path)
{
    settings_load(&global, path);
}

const char *settings_get_str(const char *key, const char *fallback)
{
    return settings_get(&global, key, fallback);
}

void settings_set_str(const char *key, const char *value)
{
    settings_put(&global, key, value);
}

int settings_get_int(const char *key, int fallback)
{
    const char *v = settings_get(&global, key, NULL);
    if (!v)
        return fallback;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end)
        return fallback;
    return (int)n;
}

void settings_set_int(const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof text, "%d", value);
    settings_set_str(key, text);
}
