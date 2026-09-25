#ifndef APICORE_H
#define APICORE_H

#include "session.h"
#include "vendor/cJSON.h"

struct apicall {
    const char   *method;
    const char   *path;
    const char   *query;
    const cJSON  *body;
    int           status;
    cJSON        *out;
    void         *ud;
};

typedef void (*apicore_reply_fn)(struct apicall *c);
typedef void (*apicore_emit_fn)(const char *event, cJSON *data);

void apicore_init(apicore_reply_fn reply, apicore_emit_fn emit);
void apicore_handle(struct apicall *c);

void apicore_tick(void);

void apicore_turn_begin(struct session *s);
void apicore_turn_done(struct session *s);

void apicore_usage(const char *backend, const backend_rate_limit *limit);

void apicore_event(void *ud, struct session *s, const backend_event *ev);

void apicore_reset(void);

#endif
