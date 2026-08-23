
#ifndef GITCMD_H
#define GITCMD_H

#include <stddef.h>

int gitcmd_line(const char *dir, const char *args, char *out, size_t size);

int gitcmd_root(const char *dir, char *out, size_t size);

#endif
