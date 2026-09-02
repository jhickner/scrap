#include "imageview.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "image.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define POLL_MS   250

static const char HINT[] =
    "\xe2\x86\x90\xe2\x86\x92 step  \xc2\xb7  x close";

struct view {
    int         n;
    int         at;
    uint32_t    id;
    int         cols, rows;         /* cells the image fills */
    int         box_cols, box_rows; /* the box it was fitted to */
};

static const char *shot_path(const struct view *v) { return image_path_at(v->at); }

static const char *shot_name(const struct view *v)
{
    const char *path = shot_path(v);
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void load(struct view *v, int cols_box, int rows_box)
{
    if (v->id && v->box_cols == cols_box && v->box_rows == rows_box)
        return;
    v->box_cols = cols_box;
    v->box_rows = rows_box;
    v->cols = v->rows = 0;
    v->id = image_load(shot_path(v), cols_box, rows_box, &v->cols, &v->rows);
}

static void paint(void *ud)
{
    struct view *v = ud;
    int          columns = ui_columns();

    int foot = chrome_foot_rows(NULL, HINT, columns);
    int body = chrome_modal_rows() - 1 - foot;
    if (body < 1)
        body = 1;

    load(v, columns > 2 ? columns - 2 : 1, body);

    char title[512];
    snprintf(title, sizeof title, "%d/%d \xc2\xb7 %s", v->at + 1, v->n, shot_name(v));
    chrome_title_paint(title);

    int rows = v->id ? v->rows : 1;
    if (rows > body)
        rows = body;
    int top = (body - rows) / 2;
    if (top < 0)
        top = 0;
    for (int i = 0; i <= top; i++)
        ui_put("\n");

    if (v->id) {
        int indent = (columns - v->cols) / 2;
        image_place(v->id, indent > 0 ? indent : 0, v->cols, v->rows);
    } else {
        ui_pad(2);
        ui_esc(ui_style(UI_DIM));
        ui_put("[image] ");
        ui_put(shot_path(v));
        ui_esc(ui_style(UI_RESET));
    }

    for (int i = top + rows; i < body; i++)
        ui_put("\n");
    chrome_foot_paint(NULL, HINT, columns);
}

static void step(struct view *v, int delta)
{
    if (v->n < 2)
        return;
    image_drop(v->id);
    v->id = 0;
    v->at = (v->at + delta + v->n) % v->n;
}

int imageview_open(int at)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return 0;

    struct view v = {0};
    v.n = image_count();
    v.at = at;
    if (v.at < 0 || v.at >= v.n)
        return 0;

    chrome_full(1);
    chrome_modal(paint, &v);

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, POLL_MS)) {
            if (chrome_modal_interrupted())
                break;
            /* the session behind the viewer keeps streaming */
            workspace_pump_quiet();
            continue;
        }

        int done = 0;
        switch (ev.key) {
        case TK_LEFT:
        case TK_SCROLL_UP:
            step(&v, -1);
            break;

        case TK_RIGHT:
        case TK_SCROLL_DOWN:
            step(&v, 1);
            break;

        case TK_RESIZE:
            v.id = 0;
            break;

        case TK_ESCAPE:
        case TK_ENTER:
        case TK_MOUSE_DOWN:
        case TK_EOF:
            done = 1;
            break;

        case TK_CHAR:
            done = ev.cp == 'x' || ev.cp == 'q' || ev.cp == ' ' || ev.cp == 3 ||
                   ev.cp == 4;
            break;

        default:
            break;
        }
        free(ev.text);
        if (done)
            break;
        chrome_paint();
    }

    image_drop(v.id);
    chrome_modal(NULL, NULL);
    chrome_full(0);
    /* the modal painted over every row, so nothing on screen can be reused */
    viewport_forget();
    viewport_touch();
    viewport_flush();
    return 1;
}

int imageview_click(int row)
{
    uint32_t id = viewport_image_at_row(row);
    return id ? imageview_open(image_index_of(id)) : 0;
}
