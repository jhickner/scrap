#ifndef TAILNET_H
#define TAILNET_H

#include <stddef.h>

#include "vendor/cJSON.h"

#define TAILNET_HOST_MAX 256

int tailnet_dir_port(void);

const char *tailnet_bind_ip(void);

const char *tailnet_self_name(void);

int tailnet_is_self(const char *host);

int tailnet_peer(const char *ip, char *host, size_t size);

const char *tailnet_split(const char *target, char *host, size_t size);

void tailnet_resolve(const char *host, char *ip, size_t size);

int tailnet_connect(const char *ip, int port, int timeout_s, char *err, size_t esize);

int tailnet_locate(const char *host, const char *target, char *ip, size_t ipsize, int *port,
                   char *id, size_t idsize, char *msg, size_t size);

int tailnet_send(const char *host, const char *from, const char *target, const char *text,
                 char *msg, size_t size);

char *tailnet_read(const char *host, const char *target, long turns, long bytes, char *msg,
                   size_t size);

int tailnet_spawn(const char *host, const char *cwd, char *target, size_t tsize, char *msg,
                  size_t size);

cJSON *tailnet_survey(void);

#endif
