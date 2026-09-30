#ifndef ASKBLOCK_H
#define ASKBLOCK_H

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

char *askblock_answer(const struct askblock *b, const int *choice,
                      const char *const *text, const char *reply);

#endif
