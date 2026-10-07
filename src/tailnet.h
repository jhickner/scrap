#ifndef TAILNET_H
#define TAILNET_H

#include <stddef.h>

#include "vendor/cJSON.h"

#define TAILNET_HOST_MAX 256

int tailnet_dir_port(void);

int tailnet_broker(void);

const char *tailnet_bind_ip(void);

int tailnet_serve(void);

int tailnet_serve_forward(int port);

const char *tailnet_self_name(void);

int tailnet_is_self(const char *host);

int tailnet_peer(const char *ip, char *host, size_t size);

const char *tailnet_split(const char *target, char *host, size_t size);

void tailnet_resolve(const char *host, char *ip, size_t size);

int tailnet_connect(const char *ip, int port, int timeout_s, char *err, size_t esize);

int tailnet_send(const char *host, const char *from, const char *target, const char *text,
                 int flags, char *msg, size_t size);

char *tailnet_read(const char *host, const char *target, long turns, long bytes, char *msg,
                   size_t size);

int tailnet_spawn(const char *host, const char *cwd, const char *prompt,
                  char *target, size_t tsize, char *msg, size_t size);

int tailnet_close(const char *host, const char *target, char *msg, size_t size);

int tailnet_rename(const char *host, const char *target, const char *title, char *msg,
                   size_t size);

struct tailnet_survey;

struct tailnet_survey *tailnet_survey_start(void);

void tailnet_survey_wait(struct tailnet_survey *s, int ms);

cJSON *tailnet_survey_result(struct tailnet_survey *s, int *version, int *pending);

int tailnet_survey_version(struct tailnet_survey *s);

void tailnet_survey_end(struct tailnet_survey *s);

cJSON *tailnet_survey(void);

#endif
