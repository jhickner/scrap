
#ifndef BOARDCARD_H
#define BOARDCARD_H

struct board_card;

enum boardcard_action {
    BOARDCARD_NONE,
    BOARDCARD_SAVE,
    BOARDCARD_UNSTART,
    BOARDCARD_APPROVE,
};

struct boardcard_edit {
    char spec[8192];
    char kind[16];
    char column[16];
    char where[4096];
    char priority[8];
    char backend[32];
    char tier[8];
    int  proposals; /* a sweep waiting on you has no spec field to save */
};

enum boardcard_action boardcard_form(const struct board_card *c,
                                     struct boardcard_edit *out);

#endif
