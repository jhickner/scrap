#include "askblock.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DASH "\xe2\x80\x94"

static char *trimmed(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    return strndup(s, n);
}

static const char *numbered(const char *line)
{
    const char *p = line;
    if (!isdigit((unsigned char)*p))
        return NULL;
    while (isdigit((unsigned char)*p))
        p++;
    if ((*p != '.' && *p != ')') || !isspace((unsigned char)p[1]))
        return NULL;
    return p + 1;
}

static const char *bulleted(const char *line)
{
    if ((*line == '-' || *line == '*') && isspace((unsigned char)line[1]))
        return line + 1;
    return NULL;
}

static int add_option(struct askq *q, const char *s)
{
    char **label = realloc(q->label, (size_t)(q->nopt + 1) * sizeof *label);
    if (!label)
        return 0;
    q->label = label;
    char **detail = realloc(q->detail, (size_t)(q->nopt + 1) * sizeof *detail);
    if (!detail)
        return 0;
    q->detail = detail;

    const char *sep = strstr(s, " " DASH " ");
    size_t      skip = sizeof DASH + 1;
    if (!sep) {
        sep = strstr(s, " - ");
        skip = 3;
    }
    label[q->nopt] = trimmed(s, sep ? (size_t)(sep - s) : strlen(s));
    detail[q->nopt] = sep ? trimmed(sep + skip, strlen(sep + skip)) : NULL;
    q->nopt++;
    return 1;
}

struct askblock *askblock_parse(const char *reply)
{
    if (!reply)
        return NULL;

    const char *at = NULL;
    for (const char *p = reply; p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        if (strncmp(s, "@ask", 4))
            continue;
        s += 4;
        while (*s == ' ' || *s == '\t' || *s == '\r')
            s++;
        if (*s == '\n' || !*s)
            at = *s ? s + 1 : s;
    }
    if (!at)
        return NULL;

    struct askblock *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;

    for (const char *p = at; *p;) {
        const char *nl = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        char       *line = trimmed(p, len);
        p = nl ? nl + 1 : p + len;
        if (!line)
            break;

        const char *rest;
        int         ok = 1;
        if (!*line) {
        } else if ((rest = numbered(line))) {
            struct askq *q = realloc(b->q, (size_t)(b->n + 1) * sizeof *q);
            if (q) {
                b->q = q;
                memset(&q[b->n], 0, sizeof *q);
                q[b->n].text = trimmed(rest, strlen(rest));
                b->n++;
            }
        } else if ((rest = bulleted(line)) && b->n) {
            add_option(&b->q[b->n - 1], rest);
        } else {
            ok = 0;
        }
        free(line);
        if (!ok)
            break;
    }

    if (!b->n) {
        askblock_free(b);
        return NULL;
    }
    return b;
}

void askblock_free(struct askblock *b)
{
    if (!b)
        return;
    for (int i = 0; i < b->n; i++) {
        struct askq *q = &b->q[i];
        for (int j = 0; j < q->nopt; j++) {
            free(q->label[j]);
            free(q->detail[j]);
        }
        free(q->label);
        free(q->detail);
        free(q->text);
    }
    free(b->q);
    free(b);
}

struct buf {
    char  *s;
    size_t n;
    size_t cap;
};

static void put(struct buf *o, const char *s)
{
    size_t k = strlen(s);
    if (o->n + k + 1 > o->cap) {
        size_t cap = (o->n + k + 1) * 2;
        char  *grown = realloc(o->s, cap);
        if (!grown)
            return;
        o->s = grown;
        o->cap = cap;
    }
    memcpy(o->s + o->n, s, k + 1);
    o->n += k;
}

char *askblock_answer(const struct askblock *b, const int *choice,
                      const char *const *text, const char *reply)
{
    struct buf o = {0};
    for (int i = 0; i < b->n; i++) {
        const char *label = choice[i] >= 0 && choice[i] < b->q[i].nopt
                                ? b->q[i].label[choice[i]] : NULL;
        const char *typed = text[i] && *text[i] ? text[i] : NULL;
        if (!label && !typed)
            continue;
        char num[16];
        snprintf(num, sizeof num, "%d. ", i + 1);
        if (o.n)
            put(&o, "\n");
        put(&o, num);
        if (label)
            put(&o, label);
        if (label && typed)
            put(&o, " " DASH " ");
        if (typed)
            put(&o, typed);
    }
    if (reply && *reply) {
        if (o.n)
            put(&o, "\n\n");
        put(&o, reply);
    }
    return o.s;
}
