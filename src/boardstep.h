#ifndef BOARDSTEP_H
#define BOARDSTEP_H

#include <stddef.h>

struct board_action;
struct board_card;

/* An action's prompt names the card's paths and branches in braces --
   {branch}, {worktree}, {repo}, {onto}, {base} -- so a command in the file
   reaches the worker with nothing left to work out. */
char *boardstep_fill(const char *text, const struct board_card *c);

int boardstep_where(const struct board_card *c, const struct board_action *p,
                    char *out, size_t size);

char *boardstep_prompt(const struct board_card *c, const struct board_action *p);

#endif
