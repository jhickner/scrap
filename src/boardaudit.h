
#ifndef BOARDAUDIT_H
#define BOARDAUDIT_H

struct board_card;

int boardaudit_size(const struct board_card *c, int *files, int *lines);

int boardaudit_wanted(const struct board_card *c);

int boardaudit_start(const struct board_card *c);
int boardaudit_running(const char *id);

int boardaudit_pump(void);

int boardaudit_take(const char *key, const char *reply);

#endif
