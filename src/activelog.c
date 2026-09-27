#include "activelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text.h"

#define LOG_MAX  (64 * 1024)
#define LOG_KEEP (16 * 1024)
#define LENGTH_MAX 256

struct entry {
    char *id;
    int   back;
};

static struct entry parse(char *line)
{
    struct entry e = {NULL, 0};
    char *id = strchr(line, '\t');
    if (!id)
        return e;
    *id++ = '\0';
    char *back = strchr(id, '\t');
    if (back) {
        *back++ = '\0';
        e.back = atoi(back);
    }
    e.id = id;
    return e;
}

static int split(char *text, char ***out)
{
    int n = 0;
    for (char *c = text; *c; c++)
        n += *c == '\n';
    char **lines = malloc(((size_t)n + 1) * sizeof *lines);
    if (!lines)
        return -1;
    n = 0;
    for (char *line = text, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl)
            break;
        *nl = '\0';
        lines[n++] = line;
    }
    *out = lines;
    return n;
}

static int put_tail(FILE *f, void *ud)
{
    return fputs(ud, f) >= 0;
}

static int last_is(const char *path, const char *id)
{
    size_t len = 0;
    char *text = text_slurp(path, 0, &len);
    if (!text)
        return 0;
    while (len && text[len - 1] == '\n')
        text[--len] = '\0';
    char *line = strrchr(text, '\n');
    struct entry e = parse(line ? line + 1 : text);
    int same = e.id && !strcmp(e.id, id);
    free(text);
    return same;
}

void activelog_add(const char *path, const char *id, int back)
{
    if (!path || !id || !*id || (!back && last_is(path, id)))
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    if (back)
        fprintf(f, "%ld\t%s\t%d\n", (long)time(NULL), id, back);
    else
        fprintf(f, "%ld\t%s\n", (long)time(NULL), id);
    long size = ftell(f);
    fclose(f);
    if (size <= LOG_MAX)
        return;

    size_t len = 0;
    char *text = text_slurp(path, 0, &len);
    char *tail = text && len > LOG_KEEP ? strchr(text + len - LOG_KEEP, '\n') : NULL;
    if (tail)
        text_spit(path, put_tail, tail + 1);
    free(text);
}

int activelog_step(const char *path, const char *here, int dir, int length,
                   int (*alive)(const char *id, void *ud), void *ud,
                   char *out, size_t size)
{
    size_t len = 0;
    char *text = path ? text_slurp(path, 0, &len) : NULL;
    char **lines = NULL;
    int n = text ? split(text, &lines) : -1;
    if (n < 0) {
        free(text);
        return 0;
    }

    struct entry *all = malloc(((size_t)n + 1) * sizeof *all);
    if (!all) {
        free(lines);
        free(text);
        return 0;
    }
    for (int i = 0; i < n; i++)
        all[i] = parse(lines[i]);

    struct entry last = n ? all[n - 1] : (struct entry){NULL, 0};
    const char *order[LENGTH_MAX];
    if (length < 1 || length > LENGTH_MAX)
        length = LENGTH_MAX;
    int count = 0;
    for (int i = n - 1; i >= 0 && count < length; i--) {
        struct entry e = all[i];
        int stopped = i + 1 < n && all[i + 1].id && !all[i + 1].back;
        if (!e.id || !*e.id || (e.back && !stopped))
            continue;
        int seen = 0;
        for (int k = 0; k < count && !seen; k++)
            seen = !strcmp(order[k], e.id);
        if (!seen)
            order[count++] = e.id;
    }

    int at = last.id && last.back && !strcmp(last.id, here) ? last.back - 1 : -1;
    int found = -1;
    if (dir < 0) {
        for (int j = at + 1; j < count && found < 0; j++)
            if (strcmp(order[j], here) && alive(order[j], ud))
                found = j;
    } else if (at >= 0) {
        for (int j = at - 1; j >= 0 && found < 0; j--)
            if (strcmp(order[j], here) && alive(order[j], ud))
                found = j;
    }
    if (found >= 0)
        snprintf(out, size, "%s", order[found]);
    free(all);
    free(lines);
    free(text);
    if (found < 0)
        return 0;
    activelog_add(path, out, found + 1);
    return 1;
}
