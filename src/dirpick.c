#include "dirpick.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pick.h"

#define DIR_MAX   6000
#define DIR_DEPTH 3

static const char *SKIP[] = {"node_modules", "target", "build",  "dist",
                             "venv",         ".git",   "Library", "__pycache__"};

struct list {
    struct pick_item *items;
    char            **shown;
    int               count, cap;
};

static int skipped(const char *name)
{
    if (name[0] == '.')
        return 1;
    for (size_t i = 0; i < sizeof SKIP / sizeof *SKIP; i++)
        if (!strcmp(name, SKIP[i]))
            return 1;
    return 0;
}

static void list_add(struct list *l, const char *shown)
{
    if (l->count >= DIR_MAX)
        return;
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        struct pick_item *items = realloc(l->items, (size_t)cap * sizeof *items);
        char            **names = realloc(l->shown, (size_t)cap * sizeof *names);
        if (items)
            l->items = items;
        if (names)
            l->shown = names;
        if (!items || !names)
            return;
        l->cap = cap;
    }
    char *copy = strdup(shown);
    if (!copy)
        return;
    l->shown[l->count] = copy;
    l->items[l->count].label = copy;
    l->items[l->count].detail = NULL;
    l->count++;
}

static void walk(struct list *l, const char *path, const char *shown, int depth)
{
    if (depth > DIR_DEPTH)
        return;

    DIR *d = opendir(path);
    if (!d)
        return;

    struct dirent *e;
    while (l->count < DIR_MAX && (e = readdir(d))) {
        if (skipped(e->d_name))
            continue;

        char full[4096], label[4096];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", path, e->d_name) >= sizeof full)
            continue;
        if ((size_t)snprintf(label, sizeof label, "%s/%s", shown, e->d_name) >= sizeof label)
            continue;

        if (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN && e->d_type != DT_LNK)
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        list_add(l, label);
        walk(l, full, label, depth + 1);
    }
    closedir(d);
}

static int depth_of(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '/')
            n++;
    return n;
}

static int cmp_item(const void *a, const void *b)
{
    const struct pick_item *x = a, *y = b;
    int dx = depth_of(x->label), dy = depth_of(y->label);
    return dx != dy ? dx - dy : strcmp(x->label, y->label);
}

char *dirpick_run(const char *title, const char *seed)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;

    struct list l = {0};
    list_add(&l, "~");
    walk(&l, home, "~", 1);
    if (!l.count)
        return NULL;
    qsort(l.items, (size_t)l.count, sizeof *l.items, cmp_item);

    int   which = pick_run_query(title, l.items, l.count, 0, seed);
    char *out = NULL;
    if (which >= 0) {
        const char *label = l.items[which].label;
        size_t      n = strlen(home) + strlen(label) + 1;
        out = malloc(n);
        if (out)
            snprintf(out, n, "%s%s", home, label + 1);
    }

    for (int i = 0; i < l.count; i++)
        free(l.shown[i]);
    free(l.shown);
    free(l.items);
    return out;
}
