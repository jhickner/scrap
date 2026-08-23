
#ifndef GITCMD_H
#define GITCMD_H

#include <stddef.h>

// One line of git's answer, trimmed. Zero when it said nothing, which for
// most questions means the answer is no.
int gitcmd_line(const char *dir, const char *args, char *out, size_t size);

// The repo proper, not whichever worktree of it `dir` happens to be. The first
// line of `worktree list` is always the main one, wherever it is asked from.
int gitcmd_root(const char *dir, char *out, size_t size);

#endif
