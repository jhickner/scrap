#ifndef ACTIVELOG_H
#define ACTIVELOG_H

#include <stddef.h>

void activelog_add(const char *path, const char *id, int back);

int activelog_step(const char *path, const char *here, int dir,
                   int (*alive)(const char *id, void *ud), void *ud,
                   char *out, size_t size);

#endif
