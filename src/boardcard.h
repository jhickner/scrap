
#ifndef BOARDCARD_H
#define BOARDCARD_H

struct board_card;

enum boardcard_action {
    BOARDCARD_NONE,
    BOARDCARD_SAVE,
    BOARDCARD_UNSTART,
    BOARDCARD_RUN,
    BOARDCARD_CLOSE,
};

struct boardcard_edit {
    char spec[8192];
    char kind[16];
    char where[4096];
    char priority[8];
    char backend[32];
    char tier[8];
    char run[32]; /* the action BOARDCARD_RUN asks for */
};

enum boardcard_action boardcard_form(const struct board_card *c,
                                     struct boardcard_edit *out);

#endif
