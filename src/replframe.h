#ifndef REPLFRAME_H
#define REPLFRAME_H

#include <stdint.h>

#include "vendor/repl.h"

// A repl rendered into cells. Its wrapping and its cursor are only knowable by
// letting it draw, so both the prompt and the card form draw one and read the
// grid back. What each does with the rows differs; the grid does not.

#define REPLFRAME_NONE ((signed char)-1)

struct replframe_cell {
    uint32_t    cp;
    signed char style;      /* a ReplStyle, or REPLFRAME_NONE for untouched */
};

struct replframe {
    struct replframe_cell *cells;
    int rows, cols, cap;
    int cursor_x, cursor_y;
    int have_cursor;
};

// Draws `r` into the frame at `rows` by `cols`, growing it as needed. Nonzero
// once the grid holds it; zero leaves the frame unusable and nothing drawn.
int replframe_render(struct replframe *f, const Repl *r, int rows, int cols,
                     int focused);

// How far along row `y` anything was drawn, so trailing blanks are not painted.
int replframe_extent(const struct replframe *f, int y);

const struct replframe_cell *replframe_at(const struct replframe *f, int y, int x);

// The escape sequence a cell's style is painted in.
const char *replframe_style(signed char style);

void replframe_free(struct replframe *f);

#endif
