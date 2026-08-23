
#ifndef GITCMD_H
#define GITCMD_H

#include <stddef.h>

int gitcmd_line(const char *dir, const char *args, char *out, size_t size);

int gitcmd_run(const char *dir, const char *args);

int gitcmd_root(const char *dir, char *out, size_t size);

int gitcmd_worktree_add(const char *root, const char *path, const char *branch);

#endif
