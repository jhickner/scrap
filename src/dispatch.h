#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>

void dispatch_poll(void);

int dispatch_fds(int *out, int max);

#include "vendor/cJSON.h"

struct dispatch_net {
    const char *(*bind)(void);
    int         port;
    int         (*peer)(const char *ip, char *host, size_t size);
    char       *(*serve)(const cJSON *o, int fd, int *kept);
};

void dispatch_net(const struct dispatch_net *config);

int dispatch_net_port(void);

int dispatch_dir(char *out, size_t size);

int dispatch_socket_path(long pid, char *out, size_t size);

int dispatch_request(long pid, const char *json, char *out, size_t size, int wait_s);

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *resume, const char *const *env,
                   const char *prompt);

int dispatch_send(int at, const char *line);

#endif
