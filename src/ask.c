#include "ask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "replbox.h"
#include "tty.h"
#include "ui.h"

#define ASK_INDENT 2
#define ASK_GUTTER 2

struct field {
    const char     *title;
    struct replbox  box;
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
    int rows = chrome_modal_rows() - 1;
    return rows < 1 ? 1 : rows;
}

static void paint(void *ud)
{
    struct field *f = ud;
    int           columns = ui_columns();

    replbox_width(&f->box, width_of());

    int rows = replbox_wants(&f->box);
    if (!replbox_render(&f->box, rows))
        return;

    int room = room_for();
    replbox_scroll(&f->box, room);
    int top = replbox_top(&f->box);

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        char        said[256];
        const char *title = f->title ? f->title : "";
        if (rows > room)
            snprintf(said, sizeof said, "%s \xc2\xb7 %d\xe2\x80\x93%d of %d", title,
                     top + 1, top + room, rows);
        else
            snprintf(said, sizeof said, "%s", title);
        ui_putn(said, ui_fit_bytes(said, (size_t)(columns > 3 ? columns - 3 : 1)));
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    int end = rows <= room ? rows : top + room;
    for (int y = top; y < end; y++) {
        ui_pad(ASK_INDENT);
        replbox_paint_row(&f->box, y, ASK_GUTTER, 1);
        ui_put("\n");
    }

    ui_esc(ui_style(UI_DIM));
    ui_pad(ASK_INDENT);
    ui_put("shift-enter newline  \xc2\xb7  enter confirm  \xc2\xb7  esc cancel");
    ui_esc(ui_style(UI_RESET));
}

static char *leave(struct field *f, char *out)
{
    chrome_modal(NULL, NULL);
    replbox_free(&f->box);
    return out;
}

char *ask_run(const char *title, const char *initial)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return NULL;

    struct field f = {.title = title ? title : ""};
    replbox_init(&f.box, NULL, 0);
    replbox_width(&f.box, width_of());
    replbox_set_text(&f.box, initial);

    chrome_modal(paint, &f);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            return leave(&f, NULL);
        }

        switch (ev.key) {
        case TK_ENTER:
            return leave(&f, strdup(replbox_line(&f.box)));

        case TK_ESCAPE:
        case TK_EOF:
            return leave(&f, NULL);

        default:
            if (ev.key == TK_CHAR && (ev.cp == 3 || ev.cp == 4))
                return leave(&f, NULL);
            replbox_key(&f.box, &ev);
            free(ev.text);
            break;
        }
        chrome_paint();
    }
}
