#include "gitinfo.h"
#include "gitcmd.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gitinfo  shown;
static char            shown_dir[4096];

static void parse_shortstat(const char *line, struct gitinfo *g)
{
    for (const char *p = line; *p; p++) {
        if (*p < '0' || *p > '9')
            continue;
        char *end;
        long value = strtol(p, &end, 10);
        p = end;
        while (*p == ' ')
            p++;
        if (strncmp(p, "insertion", 9) == 0)
            g->added = value;
        else if (strncmp(p, "deletion", 8) == 0)
            g->removed = value;
        if (!*p)
            break;
        p--;
    }
}

static void reread(const char *dir, struct gitinfo *g)
{
    memset(g, 0, sizeof *g);

    if (gitcmd_line(dir, "rev-parse --abbrev-ref HEAD", g->branch, sizeof g->branch)) {
        if (strcmp(g->branch, "HEAD") == 0)
            g->branch[0] = '\0';
        g->repo = 1;
    }
    if (gitcmd_line(dir, "rev-parse --short HEAD", g->sha, sizeof g->sha))
        g->repo = 1;

    char buf[1024];
    if (gitcmd_line(dir, "diff --shortstat HEAD", buf, sizeof buf))
        parse_shortstat(buf, g);

    g->dirty = g->repo && !gitcmd_run(dir, "diff --quiet HEAD");
    if (gitcmd_line(dir, "ls-files --others --exclude-standard", buf, sizeof buf))
        g->untracked = 1;
}

void gitinfo_forget(void)
{
    pthread_mutex_lock(&lock);
    shown_dir[0] = '\0';
    pthread_mutex_unlock(&lock);
}

const struct gitinfo *gitinfo_get(const char *dir)
{
    pthread_mutex_lock(&lock);
    if (dir && *dir && shown_dir[0] && strcmp(shown_dir, dir) == 0) {
        pthread_mutex_unlock(&lock);
        return &shown;
    }
    pthread_mutex_unlock(&lock);

    struct gitinfo g;
    if (!dir || !*dir)
        memset(&g, 0, sizeof g);
    else
        reread(dir, &g);

    pthread_mutex_lock(&lock);
    shown = g;
    if (dir && *dir)
        snprintf(shown_dir, sizeof shown_dir, "%s", dir);
    else
        shown_dir[0] = '\0';
    pthread_mutex_unlock(&lock);
    return &shown;
}
