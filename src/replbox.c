#include "replbox.h"

#include <string.h>

#include "replkeys.h"

#define KEY_CTRL(c) ((c) - 'A' + 1)

void replbox_init(struct replbox *b, const ReplCommand *cmds, int n)
{
    memset(b, 0, sizeof *b);
    repl_init(&b->repl, cmds, n);
    b->cols = 8;
    repl_set_width(&b->repl, b->cols);
}

void replbox_free(struct replbox *b)
{
    repl_free(&b->repl);
    replframe_free(&b->frame);
    memset(b, 0, sizeof *b);
}

void replbox_width(struct replbox *b, int cols)
{
    if (cols < 8)
        cols = 8;
    if (cols == b->cols)
        return;
    b->cols = cols;
    b->fresh = 0;
    repl_set_width(&b->repl, cols);
}

int replbox_wants(const struct replbox *b)
{
    int rows = repl_input_rows(&b->repl, b->cols);
    return rows < 1 ? 1 : rows;
}

int replbox_render(struct replbox *b, int rows)
{
    if (rows < 1)
        rows = 1;
    if (b->fresh && b->rows == rows)
        return 1;
    if (!replframe_render(&b->frame, &b->repl, rows, b->cols, 1))
        return 0;
    b->rows = rows;
    b->fresh = 1;
    return 1;
}

static int replbox_caret(const struct replbox *b)
{
    if (!b->fresh || !b->frame.have_cursor)
        return -1;
    return b->frame.cursor_y;
}

void replbox_scroll(struct replbox *b, int room)
{
    if (room < 1)
        room = 1;
    if (b->rows <= room) {
        b->top = 0;
        return;
    }
    int caret = replbox_caret(b);
    if (caret < 0)
        caret = b->rows - 1;
    if (caret < b->top)
        b->top = caret;
    if (caret >= b->top + room)
        b->top = caret - room + 1;
    if (b->top > b->rows - room)
        b->top = b->rows - room;
    if (b->top < 0)
        b->top = 0;
}

int replbox_top(const struct replbox *b)
{
    return b->top;
}

void replbox_paint_row(const struct replbox *b, int y, int gutter, int focused)
{
    const Repl *r = &b->repl;
    replframe_paint_row(&b->frame, y, gutter, focused,
                        r->cursor >= r->len || r->buf[r->cursor] == '\n');
}

int replbox_key(struct replbox *b, const tty_event *ev)
{
    if (ev->key == TK_CHAR && ev->cp == KEY_CTRL('V')) {
        replkeys_paste(&b->repl);
        b->fresh = 0;
        return 1;
    }
    ReplEvent re;
    if (!replkeys_map(ev, &re))
        return 0;
    repl_set_width(&b->repl, b->cols);
    repl_handle_input(&b->repl, &re);
    b->fresh = 0;
    return 1;
}

const char *replbox_line(const struct replbox *b)
{
    const char *line = repl_line(&b->repl);
    return line ? line : "";
}

void replbox_set_text(struct replbox *b, const char *text)
{
    if (!text || !*text)
        return;
    repl_insert_text(&b->repl, text);
    b->fresh = 0;
}
