
#ifndef CHILD_H
#define CHILD_H

#include <stddef.h>

#define CHILD_KEY_MAX 32

int child_start(const char *key, char *const argv[], const char *cwd);

int child_running(const char *key);

int child_reap(char *key, size_t keysize, char **out, int *ok);

int child_stop(const char *key);

void child_close_all(void);

#endif
