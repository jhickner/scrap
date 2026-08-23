
#ifndef BOARDSWEEP_H
#define BOARDSWEEP_H

#include <stddef.h>

struct board_card;

int boardsweep_due(const struct board_card *cards, int n, char *cwd, size_t size);

char *boardsweep_prompt(const struct board_card *cards, int n, const char *cwd);

int boardsweep_open(const char *cwd, char *id, size_t size);

int boardsweep_is(const struct board_card *c);

int boardsweep_proposed(const struct board_card *c);

int boardsweep_finished(const char *id, const char *reply);

int boardsweep_approve(const struct board_card *c);

int boardsweep_reject(const struct board_card *c);

#endif
