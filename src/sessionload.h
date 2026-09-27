#ifndef SESSIONLOAD_H
#define SESSIONLOAD_H

#include <stddef.h>

int sessionload_path(const char *backend, const char *cwd, const char *id,
                     char *out, size_t size);

int sessionload_replay(const char *backend, const char *cwd, const char *id,
                       int thinking);

struct session;
int sessionload_into(const struct session *s);

struct transcript;
int sessionload_fill(struct transcript *t, const char *backend, const char *cwd,
                     const char *id);

int sessionload_context(const char *backend, const char *cwd, const char *id, long *tokens,
                        long *window);

#endif
