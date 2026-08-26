#ifndef BOARDLOG_H
#define BOARDLOG_H

#include <stddef.h>

int boardlog_path(const char *id, char *out, size_t size);

void boardlog_worktree(const char *id, const char *path);

void boardlog_turn(const char *id, const char *stage, const char *prompt,
                   const char *reply);

void boardlog_note(const char *id, const char *who, const char *text);

void boardlog_remove(const char *id);

#endif
