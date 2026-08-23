
#ifndef BOARDMERGE_H
#define BOARDMERGE_H

#include <stddef.h>

struct board_card;

// Landing the work. Serialized, one card at a time: parallel workers all
// branched off the same base do not land cleanly on their own, and the whole
// reason the column exists is that somebody has to go first.
//
// Rebase onto the base branch, run the check where there is one, merge, and
// drop the worktree. Anything that fails sends the card back to be worked on
// with the output kept, and leaves the worktree where it is.

// Starts the next card in `merging` if nothing is already landing. Nonzero
// once one is going.
// The branch a card is going back onto, which is whatever the repo itself has
// checked out. Zero when that cannot be worked out.
int boardmerge_base(const struct board_card *c, char *out, size_t size);

int boardmerge_pump(void);

// Whether this card is the one currently landing.
int boardmerge_running(const char *id);

// Offered every child that has finished. Nonzero when it was ours.
int boardmerge_take(const char *key, const char *out, int ok);

#endif
