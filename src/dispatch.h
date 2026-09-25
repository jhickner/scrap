#ifndef DISPATCH_H
#define DISPATCH_H

void dispatch_poll(void);

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *const *env, const char *prompt);

int dispatch_send(int at, const char *line);

#endif
