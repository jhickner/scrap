#include "tgartifacts.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define HTTPD_IMPLEMENTATION
#include "vendor/httpd.h"

struct tgartifacts {
    httpd *server;
    char   dir[4096];
    char   base[300];
};

struct artifact {
    char   name[256];
    time_t mtime;
};

static int newer(const void *a, const void *b)
{
    const struct artifact *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;
}

static void appendf(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    *n = (size_t)w < size - *n ? *n + (size_t)w : size - 1;
}

struct tgartifacts *tgartifacts_start(const char *dir, const char *bind, int port,
                                      const char *url, const char *token)
{
    if (!dir || !*dir || !bind || !*bind || !token || !*token)
        return NULL;

    struct tgartifacts *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    snprintf(a->dir, sizeof a->dir, "%s", dir);
    a->server = httpd_start(a->dir, bind, port, token);
    if (!a->server) {
        free(a);
        return NULL;
    }
    fcntl(a->server->fd, F_SETFD, FD_CLOEXEC);

    if (url && *url) {
        char trimmed[256];
        snprintf(trimmed, sizeof trimmed, "%s", url);
        size_t n = strlen(trimmed);
        while (n && trimmed[n - 1] == '/')
            trimmed[--n] = '\0';
        snprintf(a->base, sizeof a->base, "%s/%s", trimmed, token);
    } else {
        snprintf(a->base, sizeof a->base, "http://%s:%d/%s", bind, port, token);
    }
    return a;
}

void tgartifacts_stop(struct tgartifacts *a)
{
    if (!a)
        return;
    httpd_close(a->server);
    free(a);
}

const char *tgartifacts_base(const struct tgartifacts *a)
{
    return a ? a->base : NULL;
}

const char *tgartifacts_dir(const struct tgartifacts *a)
{
    return a ? a->dir : NULL;
}

void tgartifacts_list(const struct tgartifacts *a, char *out, size_t size)
{
    if (!size)
        return;
    out[0] = '\0';
    if (!a)
        return;

    size_t n = 0;
    appendf(out, size, &n, "%s/\n", a->base);

    struct artifact entries[64];
    int count = 0;
    DIR *d = opendir(a->dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && count < (int)(sizeof entries / sizeof *entries)) {
            if (de->d_name[0] == '.')
                continue;
            char full[4400];
            snprintf(full, sizeof full, "%s/%s", a->dir, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0)
                continue;
            snprintf(entries[count].name, sizeof entries[count].name, "%s", de->d_name);
            entries[count].mtime = st.st_mtime;
            count++;
        }
        closedir(d);
    }
    qsort(entries, (size_t)count, sizeof *entries, newer);

    if (!count)
        appendf(out, size, &n, "\nnothing published yet");
    for (int i = 0; i < count && i < 10; i++)
        appendf(out, size, &n, "\n%s/%s", a->base, entries[i].name);
    if (count > 10)
        appendf(out, size, &n, "\n… and %d more", count - 10);
}
