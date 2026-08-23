#ifndef BOARDFLOW_H
#define BOARDFLOW_H

#include "board.h"
#include "boardcfg.h"

// Which column a card goes to when a step is behind it. The steps a kind takes
// are configuration, so three callers -- a worker finishing, a person
// approving, an audit coming back clean -- would each have had to work out
// what the next one was. They ask here instead.
//
// `from` is the first step still ahead of the card. `audit_worth_it` is
// whether the diff is big enough to be worth reading, which is a question
// about this card rather than about its kind.
enum board_col boardflow_from(const char *kind, enum board_step from,
                              int audit_worth_it);

#endif
