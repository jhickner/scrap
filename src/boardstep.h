#ifndef BOARDSTEP_H
#define BOARDSTEP_H

#include "boardcfg.h"

struct board_card;

char *boardstep_prompt(const struct board_card *c, const struct board_role *p);

const char *boardstep_before(const struct board_card *c);

int boardstep_finished(const struct board_card *c, const struct board_role *p,
                       const char *reply);

#endif
