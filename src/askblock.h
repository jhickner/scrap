#ifndef ASKBLOCK_H
#define ASKBLOCK_H

#include <stddef.h>

struct askq {
    char  *text;
    int    nopt;
    char **label;
    char **detail;
};

struct askblock {
    int          n;
    struct askq *q;
};

struct askblock *askblock_parse(const char *reply);
void             askblock_free(struct askblock *b);

/* Where the block sits in reply, from its "@ask" line through its last line. */
int askblock_span(const char *reply, size_t *from, size_t *to);

char *askblock_answer(const struct askblock *b, const int *choice,
                      const char *const *text, const char *reply);

#endif
