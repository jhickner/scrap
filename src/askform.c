#include "askform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "replbox.h"
#include "tty.h"
#include "ui.h"

#define FORM_INDENT 2
#define FORM_BODY   7
#define FORM_GUTTER 2

#define KEY_CTRL(c) ((c) - 'A' + 1)

struct form {
    const struct askblock *b;
    int                   *choice;
    struct replbox        *box;
    int                    focus;
    int                    left;
};

static int body_width(void)
{
    int w = ui_columns() - FORM_BODY - 1;
    return w < 8 ? 8 : w;
}

static int wrapped_rows(const char *s, size_t budget)
{
    int    rows = 0;
    size_t n = strlen(s);
    while (n) {
        size_t skip = 0;
        size_t row = ui_wrap_row(s, n, budget, &skip, NULL);
        size_t used = row + skip;
        if (!used)
            break;
        s += used;
        n -= used < n ? used : n;
        rows++;
    }
    return rows ? rows : 1;
}

static const char *item_title(const struct form *f, int i)
{
    return i < f->b->n ? f->b->q[i].text : "reply instead";
}

static int item_rows(struct form *f, int i)
{
    int opts = i < f->b->n ? f->b->q[i].nopt : 0;
    replbox_width(&f->box[i], body_width() + FORM_GUTTER);
    return wrapped_rows(item_title(f, i), (size_t)body_width()) + opts +
           replbox_wants(&f->box[i]);
}

static int row(struct form *f)
{
    if (f->left <= 0)
        return 0;
    f->left--;
    return 1;
}

static void end_row(void) { ui_put("\n"); }

static void paint_title(struct form *f, int i)
{
    const char *s = item_title(f, i);
    size_t      n = strlen(s);
    size_t      budget = (size_t)body_width();
    int         first = 1;
    int         focused = i == f->focus;

    while ((n || first) && row(f)) {
        size_t skip = 0;
        size_t cut = ui_wrap_row(s, n, budget, &skip, NULL);
        ui_pad(FORM_INDENT);
        if (first) {
            ui_esc(ui_style(focused ? UI_ACCENT : UI_DIM));
            ui_put(focused ? "\xe2\x80\xba " : "  ");
            ui_esc(ui_style(UI_RESET));
            if (i < f->b->n) {
                char num[16];
                snprintf(num, sizeof num, "%d.", i + 1);
                ui_esc(ui_style(UI_DIM));
                ui_put(num);
                ui_esc(ui_style(UI_RESET));
                ui_pad(FORM_BODY - FORM_INDENT - 2 - (int)strlen(num));
            } else {
                ui_esc(ui_style(UI_DIM));
                ui_put("\xe2\x86\xb3");
                ui_esc(ui_style(UI_RESET));
                ui_pad(FORM_BODY - FORM_INDENT - 3);
            }
        } else {
            ui_pad(FORM_BODY - FORM_INDENT);
        }
        ui_esc(ui_style(focused ? UI_BOLD : UI_TEXT));
        ui_putn(s, cut);
        ui_esc(ui_style(UI_RESET));
        end_row();
        s += cut + skip;
        n -= cut + skip < n ? cut + skip : n;
        first = 0;
    }
}

static void paint_options(struct form *f, int i)
{
    const struct askq *q = &f->b->q[i];
    size_t             budget = (size_t)body_width();

    for (int j = 0; j < q->nopt && row(f); j++) {
        int on = f->choice[i] == j;
        ui_pad(FORM_BODY);
        ui_esc(ui_style(on ? UI_ACCENT : UI_DIM));
        ui_put(on ? "\xe2\x97\x8f " : "\xe2\x97\x8b ");
        ui_esc(ui_style(on ? UI_ACCENT : UI_TEXT));
        size_t fit = ui_fit_visible(q->label[j], strlen(q->label[j]), budget - 2);
        ui_putn(q->label[j], fit);
        ui_esc(ui_style(UI_RESET));
        size_t used = ui_cells_n(q->label[j], fit) + 2;
        if (q->detail[j] && used + 4 < budget) {
            ui_esc(ui_style(UI_DIM));
            ui_put("  ");
            ui_putn(q->detail[j],
                    ui_fit_visible(q->detail[j], strlen(q->detail[j]), budget - used - 2));
            ui_esc(ui_style(UI_RESET));
        }
        end_row();
    }
}

static void paint_field(struct form *f, int i)
{
    struct replbox *box = &f->box[i];
    int             focused = i == f->focus;
    int             rows = replbox_wants(box);

    if (!replbox_render(box, rows))
        return;
    for (int y = 0; y < rows && row(f); y++) {
        ui_pad(FORM_BODY);
        ui_esc(ui_style(UI_DIM));
        ui_put(y ? "  " : "\xe2\x9c\x8e ");
        ui_esc(ui_style(UI_RESET));
        if (!focused && !*replbox_line(box)) {
            ui_esc(ui_style(UI_DIM));
            ui_put(i < f->b->n ? "type an answer" : "type a reply");
            ui_esc(ui_style(UI_RESET));
        } else {
            replbox_paint_row(box, y, FORM_GUTTER, focused);
        }
        end_row();
    }
}

static void paint(void *ud)
{
    struct form *f = ud;
    int          items = f->b->n + 1;
    int          room = chrome_modal_rows() - 2;
    if (room < 1)
        room = 1;

    int start = 0, used = 0;
    for (int i = 0; i <= f->focus; i++)
        used += item_rows(f, i);
    if (used > room) {
        start = f->focus;
        used = item_rows(f, start);
        while (start > 0 && used + item_rows(f, start - 1) <= room)
            used += item_rows(f, --start);
    }

    char title[64];
    snprintf(title, sizeof title, "%d question%s", f->b->n, f->b->n == 1 ? "" : "s");
    chrome_title_paint(title);

    f->left = room;
    for (int i = start; i < items && f->left > 0; i++) {
        paint_title(f, i);
        if (i < f->b->n)
            paint_options(f, i);
        paint_field(f, i);
    }

    ui_esc(ui_style(UI_DIM));
    ui_pad(FORM_INDENT);
    ui_put("\xe2\x86\x91\xe2\x86\x93 question  \xc2\xb7  \xe2\x86\x90\xe2\x86\x92 option  "
           "\xc2\xb7  type to answer  \xc2\xb7  enter next/send  \xc2\xb7  esc dismiss");
    ui_esc(ui_style(UI_RESET));
}

static char *finish(struct form *f, int send)
{
    chrome_modal(NULL, NULL);
    char *out = NULL;
    if (send) {
        const char **text = calloc((size_t)f->b->n + 1, sizeof *text);
        if (text) {
            for (int i = 0; i < f->b->n; i++)
                text[i] = replbox_line(&f->box[i]);
            out = askblock_answer(f->b, f->choice, text, replbox_line(&f->box[f->b->n]));
            free(text);
        }
    }
    for (int i = 0; i <= f->b->n; i++)
        replbox_free(&f->box[i]);
    free(f->box);
    free(f->choice);
    return out;
}

static void cycle(struct form *f, int dir)
{
    int n = f->b->q[f->focus].nopt;
    int c = f->choice[f->focus] + 1 + dir;
    f->choice[f->focus] = (c + n + 1) % (n + 1) - 1;
}

char *askform_run(const struct askblock *b)
{
    if (!b || !b->n || !frontend_has_keyboard() || !tty_is_raw())
        return NULL;

    struct form f = {.b = b};
    f.choice = malloc((size_t)b->n * sizeof *f.choice);
    f.box = calloc((size_t)b->n + 1, sizeof *f.box);
    if (!f.choice || !f.box) {
        free(f.choice);
        free(f.box);
        return NULL;
    }
    for (int i = 0; i < b->n; i++)
        f.choice[i] = -1;
    for (int i = 0; i <= b->n; i++)
        replbox_init(&f.box[i], NULL, 0);

    chrome_modal(paint, &f);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            return finish(&f, 0);
        }

        struct replbox *box = &f.box[f.focus];
        int             asking = f.focus < b->n;
        int             empty = !*replbox_line(box);

        switch (ev.key) {
        case TK_ESCAPE:
        case TK_EOF:
            return finish(&f, 0);

        case TK_ENTER:
            if (!asking)
                return finish(&f, 1);
            f.focus++;
            break;

        case TK_UP:
        case TK_PREV_TAB:
            if (f.focus > 0)
                f.focus--;
            break;

        case TK_DOWN:
        case TK_TAB:
        case TK_NEXT_TAB:
            if (f.focus < b->n)
                f.focus++;
            break;

        case TK_LEFT:
        case TK_RIGHT:
            if (asking && empty && b->q[f.focus].nopt)
                cycle(&f, ev.key == TK_LEFT ? -1 : 1);
            else
                replbox_key(box, &ev);
            break;

        default:
            if (ev.key == TK_CHAR && (ev.cp == KEY_CTRL('C') || ev.cp == KEY_CTRL('D')))
                return finish(&f, 0);
            replbox_key(box, &ev);
            free(ev.text);
            break;
        }
        chrome_paint();
    }
}
