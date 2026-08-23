
#ifndef BOARDWORK_H
#define BOARDWORK_H

struct board_card;
struct session;

// Workers. A worker is a tab: workspace.c already runs a turn on every tab at
// once, publishes each to the live list, colours the tab strip by status and
// calls back when a turn ends. The board spawns tabs and reads that; it does
// not add a second kind of running thing.
//
// Each card gets a worktree of its own, made here rather than asked of the
// agent, so the spawn can hand it a directory that already exists.

void boardwork_begin(void);

// Called when any tab's turn ends, whichever tab it was. A tab that is not a
// worker is left alone.
void boardwork_finished(struct session *s);

// Makes the worktree, writes CARD.md into it, opens a tab and sends the spec.
// Returns nonzero once the worker is going; `why` is filled in when it is not.
int boardwork_start(const struct board_card *c, char *why, int size);

// Why this card cannot start now, or zero if it can. The board asks it of
// every card that is waiting, so a row can say what it is waiting for without
// anyone having to press anything at it.
int boardwork_blocked(const struct board_card *c, char *why, int size);

int boardwork_running(void);

// The card this session is working, or NULL. What the live list asks so that a
// worker can be told from a session started by hand.
const char *boardwork_card_of(const struct session *s);

// The tab holding this card's worker, or -1.
int boardwork_tab(const char *id);

// Cards whose worker is gone without having finished. Nonzero when the store
// changed. A slot no card holds any more is one no card can wait for.
int boardwork_poll(void);

// Done with it: the tab closes and the worker is freed. `audit` sends the
// change for a second pass first; without it the card goes straight to the
// merge queue.
int boardwork_approve(const struct board_card *c, int audit);

// Not done with it: the tab closes and the card goes back for someone else,
// carrying why it bounced.
int boardwork_reject(const struct board_card *c, const char *why);

// More to do: the line goes to the worker's own tab and the card goes back to
// being worked on.
int boardwork_feedback(const struct board_card *c, const char *text);

void boardwork_close_all(void);

#endif
