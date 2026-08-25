#ifndef BOARDSTEP_H
#define BOARDSTEP_H

#include "boardcfg.h"

struct board_card;

char *boardstep_prompt(const struct board_card *c, const struct board_action *p);

#endif
