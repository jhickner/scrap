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

/* Draws the entries of a getAgentTranscriptTail reply for `bot`. With `since`
   set, only entries after since->id, or newer than since->ms when that id is
   no longer in the reply. `mark` receives the newest entry. Returns entries
   drawn. */
int grokbottail_render(const cJSON *tail, const char *bot,
                       const struct grokbottail_mark *since, double now_ms,
                       struct grokbottail_mark *mark);

/* Fetches the last `limit` entries of the session's bot and draws them, or
   only those after the session's tail mark when `only_new`. Returns entries
   drawn, or -1 when the gateway could not be read. */
int grokbottail_show(struct session *s, int limit, int only_new);

int grokbottail_applies(const struct session *s);

#endif
