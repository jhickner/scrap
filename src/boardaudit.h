
#ifndef BOARDAUDIT_H
#define BOARDAUDIT_H

struct board_card;

#define BOARDAUDIT_PASS       "no findings"
#define BOARDAUDIT_NO_VERDICT "no verdict; not held"

int boardaudit_is_marker(const char *text);

int boardaudit_size(const struct board_card *c, int *files, int *lines);

/* the same count, at most once every few seconds per card: the board redraws on
 * a tick and the diff is a fork. */
int boardaudit_size_cached(const struct board_card *c, int *files, int *lines);

int boardaudit_wanted(const struct board_card *c);

char *boardaudit_prompt(const struct board_card *c);

int boardaudit_finished(const char *id, const char *reply);

#endif
