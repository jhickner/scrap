#ifndef GROKBOTTAIL_H
#define GROKBOTTAIL_H

#include <stddef.h>

#include "vendor/cJSON.h"

#define GROKBOTTAIL_DEFAULT 40
#define GROKBOTTAIL_MAX     200

struct session;

struct grokbottail_mark {
    char   id[128];
    double ms;
};

int grokbottail_render(const cJSON *tail, const char *bot,
                       const struct grokbottail_mark *since, double now_ms,
                       struct grokbottail_mark *mark);

int grokbottail_show(struct session *s, int limit, int only_new);

int grokbottail_applies(const struct session *s);

#endif
