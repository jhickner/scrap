#ifndef BOARDFILE_H
#define BOARDFILE_H

#include <stddef.h>

struct board_card;

/* CARD.md in the worktree. The board writes it once when the worktree is made
   and the worker owns it from then on, so the board only ever takes a copy of
   it verbatim and puts that copy back. The copy outlives the worktree. */

int boardfile_kept(const char *id, char *out, size_t size);

void boardfile_keep(const struct board_card *c);

int boardfile_put(const char *tree, const struct board_card *c);

void boardfile_drop(const char *id);

#endif
