#include "ask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "replframe.h"
#include "replkeys.h"
#include "tty.h"
#include "ui.h"

#define ASK_INDENT 2
#define ASK_GUTTER 2

struct field {
    const char *title;
    Repl        repl;

    struct replframe frame;
    int              rows;
    int              top;
};

static int width_of(void)
{
    int budget = ui_columns() - ASK_INDENT - 2;
    if (budget < 8)
        budget = 8;
    return budget + ASK_GUTTER;
}

static int room_for(void)
{
    int rows = tty_rows() - 3 - chrome_gap();
    return rows < 1 ? 1 : rows;
}

static int framed(struct field *f)
{
    int width = width_of();
    f->rows = repl_input_rows(&f->repl, width);
    if (f->rows < 1)
        f->rows = 1;
    return replframe_render(&f->frame, &f->repl, f->rows, width, 1);
}

static void scroll_to_caret(struct field *f, int room)
{
    if (f->rows <= room) {
        f->top = 0;
        return;
    }
    int caret = f->frame.have_cursor ? f->frame.cursor_y : f->rows - 1;
    if (caret < f->top)
        f->top = caret;
    if (caret >= f->top + room)
        f->top = caret - room + 1;
    if (f->top > f->rows - room)
        f->top = f->rows - room;
    if (f->top < 0)
        f->top = 0;
}

static void paint(void *ud)
{
    struct field *f = ud;
    int           columns = ui_columns();

    if (!framed(f))
        return;

    int room = room_for();
    scroll_to_caret(f, room);

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        char        said[256];
        const char *title = f->title ? f->title : "";
        if (f->rows > room)
            snprintf(said, sizeof said, "%s \xc2\xb7 %d\xe2\x80\x93%d of %d", title,
                     f->top + 1, f->top + room, f->rows);
        else
            snprintf(said, sizeof said, "%s", title);
        ui_putn(said, ui_fit_bytes(said, (size_t)(columns > 3 ? columns - 3 : 1)));
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    int end = f->rows <= room ? f->rows : f->top + room;
    for (int y = f->top; y < end; y++) {
        ui_pad(ASK_INDENT);
        replframe_paint_row(&f->frame, y, ASK_GUTTER, 1,
                            f->repl.cursor >= f->repl.len ||
                                f->repl.buf[f->repl.cursor] == '\n');
        ui_put("\n");
    }

    ui_esc(ui_style(UI_DIM));
    ui_pad(ASK_INDENT);
    ui_put("shift-enter newline  \xc2\xb7  enter confirm  \xc2\xb7  esc cancel");
    ui_esc(ui_style(UI_RESET));
}

static void feed(struct field *f, const ReplEvent *ev)
{
    repl_set_width(&f->repl, width_of());
    repl_handle_input(&f->repl, ev);
}

static char *leave(struct field *f, char *out)
{
    chrome_modal(NULL, NULL);
    repl_free(&f->repl);
    replframe_free(&f->frame);
    return out;
}

char *ask_run(const char *title, const char *initial)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return NULL;

    struct field f = {.title = title ? title : ""};
    repl_init(&f.repl, NULL, 0);
    repl_set_width(&f.repl, width_of());
    if (initial && *initial)
        repl_insert_text(&f.repl, initial);

    chrome_modal(paint, &f);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            return leave(&f, NULL);
        }

        switch (ev.key) {
        case TK_ENTER: {
            const char *line = repl_line(&f.repl);
            return leave(&f, strdup(line ? line : ""));
        }

        case TK_ESCAPE:
        case TK_EOF:
            return leave(&f, NULL);

        default: {
            if (ev.key == TK_CHAR && (ev.cp == 3 || ev.cp == 4))
                return leave(&f, NULL);
            ReplEvent re;
            if (replkeys_map(&ev, &re))
                feed(&f, &re);
            free(ev.text);
            break;
        }
        }
        chrome_paint();
    }
}
