
#ifndef BOARDWORK_H
#define BOARDWORK_H

struct board_card;
struct session;

void boardwork_begin(void);

void boardwork_finished(struct session *s);

int boardwork_start(const struct board_card *c, char *why, int size);

int boardwork_blocked(const struct board_card *c, char *why, int size);

int boardwork_running(void);

const char *boardwork_card_of(const struct session *s);

int boardwork_tab(const char *id);

int boardwork_poll(void);

int boardwork_release(const struct board_card *c);

int boardwork_pump(void);

int boardwork_approve(const struct board_card *c, int audit);

int boardwork_reject(const struct board_card *c, const char *why);

int boardwork_feedback(const struct board_card *c, const char *text);

void boardwork_close_all(void);

#endif
