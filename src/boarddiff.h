#ifndef BOARDDIFF_H
#define BOARDDIFF_H

struct board_card;
struct board_role;

int boarddiff_size(const struct board_card *c, int *files, int *lines);

/* the same count, at most once every few seconds per card: the board redraws on
 * a tick and the diff is a fork. */
int boarddiff_size_cached(const struct board_card *c, int *files, int *lines);

int boarddiff_over(const struct board_role *p, const struct board_card *c);

#endif
