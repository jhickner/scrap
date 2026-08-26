
#ifndef BOARDWORK_H
#define BOARDWORK_H

#include <stddef.h>

#include "boardcfg.h"
#include "boardflow.h"

struct board_card;
struct session;

void boardwork_finished(struct session *s);

int boardwork_start(const struct board_card *c, char *why, int size);

int boardwork_rejoin(const struct board_card *c, char *why, int size);

int boardwork_blocked(const struct board_card *c, char *why, int size);

int boardwork_running(void);

int boardwork_serve(int *waiting);

const char *boardwork_card_of(const struct session *s);

int boardwork_tab(const char *id);

const char *boardwork_step_job(const char *id);

void boardwork_let_go(const char *id);

int boardwork_poll(void);

void boardwork_discard(const struct board_card *c);

/* how many commits closing the card would throw away with its branch */
int boardwork_unmerged(const struct board_card *c);

/* Run the action the card is on now, rather than waiting for the board's next
   tick, which only comes round while the board view is open. */
void boardwork_step(const char *id);

int boardwork_pump(void);

/* stop what the card is running and say why, leaving it in review */
int boardwork_stop(const struct board_card *c, const char *why);

void boardwork_spoke_to(struct session *s);

int boardwork_feedback(const struct board_card *c, const char *text);

#endif
