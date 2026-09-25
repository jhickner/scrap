#ifndef DISPATCH_H
#define DISPATCH_H

void dispatch_poll(void);

#include <stddef.h>

int dispatch_dir(char *out, size_t size);

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *resume, const char *const *env,
                   const char *prompt);

int dispatch_send(int at, const char *line);

#endif
