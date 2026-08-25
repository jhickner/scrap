#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

struct board_card;

const char *boardflow_next(const struct board_card *c, const char *after,
                           int force);

const char *boardflow_start(void);

const char *boardflow_after_turn(const struct board_card *c);

const char *boardflow_fail(const struct board_card *c);

enum board_runs boardflow_runs(const struct board_card *c);

int boardflow_waits_on_you(const struct board_card *c);

const struct board_role *boardflow_role(const struct board_card *c);

char *boardflow_approval(const struct board_card *c);

int boardflow_skippable(const struct board_card *c);

int boardflow_skip(const struct board_card *c);

int boardflow_lands(const char *kind);

int boardflow_worktree(const char *kind);

const char *boardflow_person(const char *kind);

#endif
