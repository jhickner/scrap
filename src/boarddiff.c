#include "boarddiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "boardcfg.h"
#include "gitcmd.h"

int boarddiff_size(const struct board_card *c, int *files, int *lines)
{
    if (files)
        *files = 0;
    if (lines)
        *lines = 0;
    if (!c || !c->worktree[0] || !c->base[0])
        return 0;

    char args[256];
    snprintf(args, sizeof args, "diff --shortstat %s..HEAD", c->base);

    char out[512];
    if (!gitcmd_line(c->worktree, args, out, sizeof out))
        return 0;

    int changed = 0, added = 0, removed = 0;
    const char *p = out;
    while (*p) {
        while (*p == ' ')
            p++;
        int         value = atoi(p);
        const char *word = p;
        while (*word && *word != ' ')
            word++;
        while (*word == ' ')
            word++;
        if (!strncmp(word, "file", 4))
            changed = value;
        else if (!strncmp(word, "insertion", 9))
            added = value;
        else if (!strncmp(word, "deletion", 8))
            removed = value;
        const char *comma = strchr(p, ',');
        if (!comma)
            break;
        p = comma + 1;
    }

    if (files)
        *files = changed;
    if (lines)
        *lines = added + removed;
    return changed > 0 || added > 0 || removed > 0;
}

#define SIZE_CACHE_MAX 32
#define SIZE_CACHE_TTL 5

struct size_entry {
    char   id[BOARD_ID_MAX];
    char   base[24];
    time_t taken;
    int    files, lines, any;
};

static struct size_entry size_cache[SIZE_CACHE_MAX];
static int               size_cache_n;

int boarddiff_size_cached(const struct board_card *c, int *files, int *lines)
{
    if (files)
        *files = 0;
    if (lines)
        *lines = 0;
    if (!c || !c->worktree[0] || !c->base[0])
        return 0;

    time_t             now = time(NULL);
    struct size_entry *e = NULL;
    for (int i = 0; i < size_cache_n; i++)
        if (!strcmp(size_cache[i].id, c->id)) {
            e = &size_cache[i];
            break;
        }

    if (e && !strcmp(e->base, c->base) && now - e->taken < SIZE_CACHE_TTL) {
        if (files)
            *files = e->files;
        if (lines)
            *lines = e->lines;
        return e->any;
    }

    int got_files = 0, got_lines = 0;
    int any = boarddiff_size(c, &got_files, &got_lines);

    if (!e) {
        if (size_cache_n < SIZE_CACHE_MAX)
            e = &size_cache[size_cache_n++];
        else {
            e = &size_cache[0];
            for (int i = 1; i < size_cache_n; i++)
                if (size_cache[i].taken < e->taken)
                    e = &size_cache[i];
        }
        snprintf(e->id, sizeof e->id, "%s", c->id);
    }
    snprintf(e->base, sizeof e->base, "%s", c->base);
    e->taken = now;
    e->files = got_files;
    e->lines = got_lines;
    e->any = any;

    if (files)
        *files = got_files;
    if (lines)
        *lines = got_lines;
    return any;
}

int boarddiff_over(const struct board_role *p, const struct board_card *c)
{
    if (!p || (!p->over_files && !p->over_lines))
        return 1;

    int files = 0, lines = 0;
    if (!boarddiff_size(c, &files, &lines))
        return 0;

    if (p->over_files && files > p->over_files)
        return 1;
    if (p->over_lines && lines > p->over_lines)
        return 1;
    return 0;
}
