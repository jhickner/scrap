#include "gitinfo.h"
#include "text.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gitinfo  shown;

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

    char quoted[4200];
    if (!text_shell_quote(dir, quoted, sizeof quoted))
        return;

    char cmd[17000];

    if (snprintf(cmd, sizeof cmd,
                 "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null; echo @; "
                 "git -C %s rev-parse --short HEAD 2>/dev/null; echo @; "
                 "git -C %s diff --shortstat HEAD 2>/dev/null; echo @; "
                 "git -C %s status --porcelain 2>/dev/null",
                 quoted, quoted, quoted, quoted) >= (int)sizeof cmd)
        return;

    FILE *f = popen(cmd, "r");
    if (!f)
        return;

    char line[1024];
    int  section = 0;
    while (fgets(line, sizeof line, f)) {
        text_chomp(line);
        if (strcmp(line, "@") == 0) {
            section++;
            continue;
        }
        if (!*line)
            continue;
        switch (section) {
        case 0:
            if (strcmp(line, "HEAD") != 0)
                snprintf(g->branch, sizeof g->branch, "%s", line);
            g->repo = 1;
            break;
        case 1:
            snprintf(g->sha, sizeof g->sha, "%s", line);
            g->repo = 1;
            break;
        case 2:
            parse_shortstat(line, g);
            break;
        default:
            if (strncmp(line, "??", 2) == 0)
                g->untracked = 1;
            else
                g->dirty = 1;
            break;
        }
    }
    pclose(f);
}

void gitinfo_forget(void) {}

const struct gitinfo *gitinfo_get(const char *dir)
{
    struct gitinfo g;

    if (!dir || !*dir) {
        memset(&g, 0, sizeof g);
    } else {
        reread(dir, &g);
    }

    pthread_mutex_lock(&lock);
    shown = g;
    pthread_mutex_unlock(&lock);
    return &shown;
}
