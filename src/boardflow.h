#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

struct board_card;

enum board_col boardflow_from(const char *kind, enum board_step from,
                              int audit_worth_it);

enum board_step boardflow_step_at(enum board_col col);

int boardflow_skippable(const struct board_card *c);

int boardflow_skip(const struct board_card *c);

#endif
