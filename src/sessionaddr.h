#ifndef SESSIONADDR_H
#define SESSIONADDR_H

#include <stddef.h>

int sessionaddr_alloc(char *out, size_t size);

void sessionaddr_write(const char *path, const char *id);

void sessionaddr_forget(const char *path);

#endif
