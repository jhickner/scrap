#ifndef APICORE_H
#define APICORE_H

#include "session.h"
#include "vendor/cJSON.h"

/* agents and runs of the worker API. main thread only: every function here
   touches workspace and session state. the HTTP layer (api.c) hands calls in
   and receives replies and events through the two callbacks */

struct apicall {
    const char   *method;
    const char   *path;   /* without the query string */
    const char   *query;  /* raw query string, or NULL */
    const cJSON  *body;   /* parsed request body, or NULL */
    int           status; /* set with out when the call completes */
    cJSON        *out;
    void         *ud;     /* the caller's */
};

/* reply is called exactly once per call, possibly from a later apicore_tick.
   emit receives ownership of data */
typedef void (*apicore_reply_fn)(struct apicall *c);
typedef void (*apicore_emit_fn)(const char *event, cJSON *data);

void apicore_init(apicore_reply_fn reply, apicore_emit_fn emit);
void apicore_handle(struct apicall *c);

/* settle held creates, notice closed tabs and status changes */
void apicore_tick(void);

void apicore_turn_begin(struct session *s);
void apicore_turn_done(struct session *s);

/* a session_add_listener callback */
void apicore_event(void *ud, struct session *s, const backend_event *ev);

/* drop all state; for tests and shutdown */
void apicore_reset(void);

#endif
