#ifndef BRIDGES_H
#define BRIDGES_H

#include <stddef.h>

void bridges_tick(void);

int bridges_wanted(const char *name);

int bridges_set(const char *name, int on, char *msg, size_t size);

#endif
