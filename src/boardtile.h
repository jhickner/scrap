#ifndef BOARDTILE_H
#define BOARDTILE_H

#include <time.h>

#include "board.h"

#define BOARD_RECENT 3

struct board_tile {
    const struct board_card *c;
    char                     title[BOARD_TITLE_MAX];
    char                     spec[512];
    char                     status[512];
    char                     pins[48];
    const char              *mark;
    unsigned char            mark_role;
    unsigned char            spin;
    int                      tab;
    time_t                   stamp;
    const char              *recent[BOARD_RECENT];
    int                      recent_n;
};

/* the step a running child is at, or NULL */
const char *boardtile_step(const char *id);

int boardtile_of(const struct board_card *c, int wide, struct board_tile *out);

#endif
