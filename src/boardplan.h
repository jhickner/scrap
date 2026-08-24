#ifndef BOARDPLAN_H
#define BOARDPLAN_H

struct board_card;

int boardplan_is(const struct board_card *c);

int boardplan_approve(const struct board_card *c);

#endif
