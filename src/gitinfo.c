#include "gitinfo.h"
#include "text.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TTL_MS 3000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gitinfo  latest;
static char            latest_dir[4096];
static double          read_at;
static int             reading;
static struct gitinfo  shown;

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

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

static void *read_thread(void *arg)
{
    char          *dir = arg;
    struct gitinfo g;
    reread(dir, &g);

    pthread_mutex_lock(&lock);
    latest = g;
    snprintf(latest_dir, sizeof latest_dir, "%s", dir);
    read_at = now_ms();
    reading = 0;
    pthread_mutex_unlock(&lock);

    free(dir);
    return NULL;
}

static void start_read(const char *dir)
{
    char *copy = strdup(dir);
    if (!copy)
        return;

    pthread_t t;
    reading = 1;
    if (pthread_create(&t, NULL, read_thread, copy) != 0) {
        reading = 0;
        free(copy);
        return;
    }
    pthread_detach(t);
}

void gitinfo_forget(void)
{
    pthread_mutex_lock(&lock);
    read_at = 0;
    pthread_mutex_unlock(&lock);
}

/* Answers from the last read and refreshes behind the caller: the four git
   commands take long enough on a large repo to be felt as a stall in the
   render they were asked from. */
const struct gitinfo *gitinfo_get(const char *dir)
{
    if (!dir || !*dir) {
        memset(&shown, 0, sizeof shown);
        return &shown;
    }

    pthread_mutex_lock(&lock);
    int here = strcmp(dir, latest_dir) == 0;
    if (here)
        shown = latest;
    else
        memset(&shown, 0, sizeof shown);
    if (!reading && (!here || read_at == 0 || now_ms() - read_at >= TTL_MS))
        start_read(dir);
    pthread_mutex_unlock(&lock);

    return &shown;
}
