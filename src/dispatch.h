#ifndef DISPATCH_H
#define DISPATCH_H

#include <stddef.h>

void dispatch_poll(void);

int dispatch_fds(int *out, int max);

int dispatch_dir(char *out, size_t size);

int dispatch_socket_path(long pid, char *out, size_t size);

int dispatch_request(long pid, const char *json, char *out, size_t size, int wait_s);

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *resume, const char *const *env,
                   const char *prompt);

int dispatch_send(int at, const char *line);

#endif
