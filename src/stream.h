#ifndef STREAM_H
#define STREAM_H

#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

struct session;

int stream_serve(const cJSON *o, int fd);

int stream_fds(int *out, int max);

void stream_poll(void);

void stream_event(void *ud, struct session *s, const backend_event *ev);

void stream_turn_begin(struct session *s);

void stream_turn_done(struct session *s);

int stream_watched(const struct session *s);

void stream_side(const struct session *s, const char *question, const char *answer,
                 int failed);

const char *stream_kind_name(int kind);

int stream_kind_of(const char *name);

#endif
