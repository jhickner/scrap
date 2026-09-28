#ifndef INTERCOM_H
#define INTERCOM_H

#include <stddef.h>

#include "vendor/cJSON.h"
#include "vendor/repl.h"

#define INTERCOM_NAME_MAX 40

void intercom_name_new(char *out, size_t size);

int intercom_name_valid(const char *name);

int intercom_name_taken(const char *name, const char *id);

int intercom_name_of(const char *id, char *out, size_t size);

void intercom_register(const char *id, const char *name, const char *backend,
                       const char *cwd);

char *intercom_note(const char *name);

int intercom_complete(void *ctx, const char *token, ReplCandidate *out, int max);

int intercom_send(const char *from, const char *target, const char *text, char *msg,
                  size_t size);

int intercom_deliver(const char *host, const char *from, const char *target, const char *text,
                     char *msg, size_t size);

char *intercom_read(const char *target, long turns, long bytes, char *msg, size_t size);

cJSON *intercom_live_json(void);

char *intercom_net_list(const char *query);

int intercom_main(int argc, char **argv);

#endif
