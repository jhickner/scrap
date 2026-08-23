
#ifndef BOARDTRIAGE_H
#define BOARDTRIAGE_H

struct board_card;

int boardtriage_start(const struct board_card *c);

int boardtriage_running(const char *id);

int boardtriage_take(const char *key, const char *reply);

int boardtriage_attempts(const struct board_card *c);

#define BOARDTRIAGE_TRIES 2

#endif
