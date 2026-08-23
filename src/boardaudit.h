
#ifndef BOARDAUDIT_H
#define BOARDAUDIT_H

struct board_card;

int boardaudit_size(const struct board_card *c, int *files, int *lines);

int boardaudit_wanted(const struct board_card *c);

char *boardaudit_prompt(const struct board_card *c);

int boardaudit_finished(const char *id, const char *reply);

#endif
