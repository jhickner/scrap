#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

struct board_card;

/* The action at the head of the card's queue, which is the one it is on. */
const char *boardflow_running(const struct board_card *c);

const struct board_action *boardflow_action(const struct board_card *c);

/* An action may run once everything it names has run on this card. */
int boardflow_gated(const struct board_card *c, const char *name);

int boardflow_offered(const struct board_card *c, const char **out, int max);

int boardflow_waits_on_you(const struct board_card *c);

/* A card gets a worktree when an action it has run or has queued asks to run
   in one, so a card that is only ever planned in the repo never makes a tree. */
int boardflow_worktree(const struct board_card *c);

const char *boardflow_cwd(const struct board_card *c,
                          const struct board_action *p);

#endif
