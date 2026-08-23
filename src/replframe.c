#include "replframe.h"

#include <limits.h>
#include <stdlib.h>

#include "ui.h"

static void size_to(struct replframe *f, int rows, int cols)
{
    if (rows < 0 || cols < 0 || (cols > 0 && rows > INT_MAX / cols)) {
        f->rows = f->cols = 0;
        return;
    }
    int need = rows * cols;
    if (need > f->cap) {
        struct replframe_cell *grown =
            realloc(f->cells, (size_t)need * sizeof *grown);
        if (!grown) {
            f->rows = f->cols = 0;
            return;
        }
        f->cells = grown;
        f->cap = need;
    }
    f->rows = rows;
    f->cols = cols;
    for (int i = 0; i < need; i++) {
        f->cells[i].cp = ' ';
        f->cells[i].style = REPLFRAME_NONE;
    }
    f->have_cursor = 0;
    f->cursor_x = f->cursor_y = 0;
}

static void draw(void *ctx, int x, int y, uint32_t cp, ReplStyle style)
{
    struct replframe *f = ctx;
    if (x < 0 || y < 0 || x >= f->cols || y >= f->rows)
        return;
    if (style == REPL_STYLE_CURSOR) {
        f->cursor_x = x;
        f->cursor_y = y;
        f->have_cursor = 1;
    }
    struct replframe_cell *c = &f->cells[y * f->cols + x];
    c->cp = cp;
    c->style = (signed char)style;
}

int replframe_render(struct replframe *f, const Repl *r, int rows, int cols,
                     int focused)
{
    size_to(f, rows, cols);
    if (!f->cells || f->rows < rows || f->cols < cols)
        return 0;
    repl_render(r, 0, 0, cols, focused != 0, draw, f);
    return 1;
}

int replframe_extent(const struct replframe *f, int y)
{
    if (!f->cells || y < 0 || y >= f->rows)
        return 0;
    int last = -1;
    for (int x = 0; x < f->cols; x++) {
        const struct replframe_cell *c = &f->cells[y * f->cols + x];
        int blank = (c->cp == ' ' &&
                     (c->style == REPLFRAME_NONE || c->style == REPL_STYLE_TYPED ||
                      c->style == REPL_STYLE_DIM));
        if (!blank)
            last = x;
    }
    return last + 1;
}

const struct replframe_cell *replframe_at(const struct replframe *f, int y, int x)
{
    if (!f->cells || y < 0 || y >= f->rows || x < 0 || x >= f->cols)
        return NULL;
    return &f->cells[y * f->cols + x];
}

const char *replframe_style(signed char style)
{
    switch (style) {
    case REPL_STYLE_PROMPT:   return ui_style(UI_CHROME);
    case REPL_STYLE_TYPED:    return ui_style(UI_TEXT);
    case REPL_STYLE_DIM:      return ui_style(UI_DIM);
    case REPL_STYLE_CURSOR:   return ui_style(UI_ACCENT);
    case REPL_STYLE_SELECTED: return ui_style(UI_ACCENT);
    default:                  return "";
    }
}

void replframe_free(struct replframe *f)
{
    free(f->cells);
    f->cells = NULL;
    f->rows = f->cols = f->cap = 0;
}
