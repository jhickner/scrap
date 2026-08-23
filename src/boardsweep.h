
#ifndef BOARDSWEEP_H
#define BOARDSWEEP_H

#include <stddef.h>

struct board_card;

int boardsweep_due(const struct board_card *cards, int n, char *id, size_t idsize,
                   char *root, size_t rootsize);

char *boardsweep_prompt(const struct board_card *cards, int n, const char *cwd);

int boardsweep_finished(const char *id, const char *reply);

int boardsweep_proposed(void);

const char *boardsweep_proposal(int at);

const char *boardsweep_proposal_repo(int at);

int boardsweep_accept(int at, const char *text);

void boardsweep_drop(int at);

void boardsweep_drop_all(void);

#endif
