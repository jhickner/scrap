#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

struct board_card;

/* The action at the head of the card's queue, which is the one it is on. */
const struct board_action *boardflow_action(const struct board_card *c);

/* An action may run once everything it names has run on this card. */
int boardflow_gated(const struct board_card *c, const char *name);

/* The actions whose gates are met, then the pipelines that would be taken
   whole. Either kind of name can be handed straight to boardflow_trigger. */
int boardflow_offered(const struct board_card *c, const char **out, int max);

/* Put actions on the end of a card's queue, in the order given. A name may be
   a pipeline, which stands for the actions it lists. */
int boardflow_trigger(const struct board_card *c, const char *const *names,
                      int n, char *why, size_t size);

/* Whether this action, passing now, leaves the card finished rather than in
   review: it says it closes the card and nothing is queued behind it. */
int boardflow_closes(const struct board_card *c, const struct board_action *p);

int boardflow_waits_on_you(const struct board_card *c);

/* A card gets a worktree when an action it has run or has queued asks to run
   in one, so a card that is only ever planned in the repo never makes a tree. */
int boardflow_worktree(const struct board_card *c);

const char *boardflow_cwd(const struct board_card *c,
                          const struct board_action *p);

#endif
