#ifndef REPLFRAME_H
#define REPLFRAME_H

#include <stdint.h>

#include "vendor/repl.h"

#define REPLFRAME_NONE ((signed char)-1)

struct replframe_cell {
    uint32_t    cp;
    signed char style;
};

struct replframe {
    struct replframe_cell *cells;
    int rows, cols, cap;
    int cursor_x, cursor_y;
    int have_cursor;
};

int replframe_render(struct replframe *f, const Repl *r, int rows, int cols,
                     int focused);

int replframe_extent(const struct replframe *f, int y);

const struct replframe_cell *replframe_at(const struct replframe *f, int y, int x);

const char *replframe_style(signed char style);

void replframe_paint_row(const struct replframe *f, int y, int from_x, int focused,
                         int hollow);

void replframe_free(struct replframe *f);

#endif
