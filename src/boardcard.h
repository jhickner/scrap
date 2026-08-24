
#ifndef BOARDCARD_H
#define BOARDCARD_H

struct board_card;

enum boardcard_action {
    BOARDCARD_NONE,
    BOARDCARD_UNSTART,
    BOARDCARD_APPROVE,
};

enum boardcard_action boardcard_form(const struct board_card *c);

#endif
