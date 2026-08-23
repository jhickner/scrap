#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

enum board_col boardflow_from(const char *kind, enum board_step from,
                              int audit_worth_it);

#endif
