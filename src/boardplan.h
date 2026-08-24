#ifndef BOARDPLAN_H
#define BOARDPLAN_H

struct board_card;

int boardplan_is(const struct board_card *c);

int boardplan_named(const char *kind);

char *boardplan_discussion(const struct board_card *c);

int boardplan_approve(const struct board_card *c);

#endif
