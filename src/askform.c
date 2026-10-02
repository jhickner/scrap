#include "askform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "chrome.h"
#include "frontend.h"
#include "replbox.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define FORM_INDENT 2
#define FORM_BODY   5
#define FORM_GUTTER 2

#define KEY_CTRL(c) ((c) - 'A' + 1)

struct form {
    const struct askblock *b;
    int                   *choice;
    struct replbox        *box;
    int                    focus;
    int                    opt;
    int                    left;
};

static int body_width(void)
{
    int w = ui_columns() - FORM_BODY - 1;
    return w < 8 ? 8 : w;
}

static int nopt(const struct form *f, int i)
{
    return i < f->b->n ? f->b->q[i].nopt : 0;
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

static char *option_text(const struct askq *q, int j, size_t *split)
{
    const char *label = q->label[j];
    const char *detail = q->detail[j] ? q->detail[j] : "";
    size_t      n = strlen(label) + strlen(detail) + 3;
    char       *s = malloc(n);
    if (s)
        snprintf(s, n, *detail ? "%s  %s" : "%s", label, detail);
    *split = strlen(label);
    return s;
}

static size_t option_budget(void) { return (size_t)body_width() - 2; }

static int option_rows(const struct form *f, int i)
{
    int rows = 0;
    for (int j = 0; j < nopt(f, i); j++) {
        size_t split;
        char  *s = option_text(&f->b->q[i], j, &split);
        rows += s ? wrapped_rows(s, option_budget()) : 1;
        free(s);
    }
    return rows;
}

static int item_rows(struct form *f, int i)
{
    replbox_width(&f->box[i], body_width());
    return wrapped_rows(item_title(f, i), (size_t)body_width()) + option_rows(f, i) +
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

static void cursor(int on)
{
    ui_pad(FORM_BODY - 2);
    ui_esc(ui_style(UI_ACCENT));
    ui_put(on ? "\xe2\x80\xba " : "  ");
    ui_esc(ui_style(UI_RESET));
}

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
            char num[16];
            if (i < f->b->n)
                snprintf(num, sizeof num, "%d.", i + 1);
            else
                snprintf(num, sizeof num, "\xe2\x86\xb3");
            ui_esc(ui_style(UI_DIM));
            ui_put(num);
            ui_esc(ui_style(UI_RESET));
            ui_pad(FORM_BODY - FORM_INDENT - (int)ui_cells(num));
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

    for (int j = 0; j < q->nopt && f->left > 0; j++) {
        int    on = f->choice[i] == j;
        int    here = f->focus == i && f->opt == j;
        size_t split;
        char  *text = option_text(q, j, &split);
        if (!text)
            return;
        size_t n = strlen(text), off = 0;
        int    first = 1;

        while ((off < n || first) && row(f)) {
            size_t skip = 0;
            size_t cut = ui_wrap_row(text + off, n - off, option_budget(), &skip, NULL);
            if (first) {
                cursor(here);
                ui_esc(ui_style(on ? UI_ACCENT : UI_DIM));
                ui_put(on ? "\xe2\x97\x8f " : "\xe2\x97\x8b ");
            } else {
                ui_pad(FORM_BODY + 2);
            }
            size_t head = off < split ? (split - off < cut ? split - off : cut) : 0;
            ui_esc(ui_style(on || here ? UI_ACCENT : UI_TEXT));
            ui_putn(text + off, head);
            ui_esc(ui_style(UI_DIM));
            ui_putn(text + off + head, cut - head);
            ui_esc(ui_style(UI_RESET));
            end_row();
            if (!cut && !skip)
                break;
            off += cut + skip < n - off ? cut + skip : n - off;
            first = 0;
        }
        free(text);
    }
}

static void paint_field(struct form *f, int i)
{
    struct replbox *box = &f->box[i];
    int             focused = i == f->focus && f->opt < 0;
    int             rows = replbox_wants(box);

    if (!replbox_render(box, rows))
        return;
    for (int y = 0; y < rows && row(f); y++) {
        cursor(focused && !y);
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
    ui_put("\xe2\x86\x91\xe2\x86\x93 move  \xc2\xb7  space choose  \xc2\xb7  type to answer  "
           "\xc2\xb7  enter next  \xc2\xb7  pgup scroll  \xc2\xb7  esc dismiss");
    ui_esc(ui_style(UI_RESET));
}

static char *finish(struct form *f, int send)
{
    chrome_modal(NULL, NULL);
    viewport_scroll_end();
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

static void enter_item(struct form *f, int i)
{
    f->focus = i;
    f->opt = nopt(f, i) ? 0 : -1;
}

static void down(struct form *f)
{
    if (f->opt >= 0 && f->opt + 1 < nopt(f, f->focus))
        f->opt++;
    else if (f->opt >= 0)
        f->opt = -1;
    else if (f->focus < f->b->n)
        enter_item(f, f->focus + 1);
}

static void up(struct form *f)
{
    if (f->opt > 0)
        f->opt--;
    else if (f->opt < 0 && nopt(f, f->focus))
        f->opt = nopt(f, f->focus) - 1;
    else if (f->focus > 0) {
        f->focus--;
        f->opt = -1;
    }
}

static int typing(const tty_event *ev)
{
    return ev->key == TK_TEXT || (ev->key == TK_CHAR && ev->cp >= ' ');
}

char *askform_run(const struct askblock *b, enum askform_exit *how)
{
    *how = ASKFORM_DONE;
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
    enter_item(&f, 0);

    chrome_modal(paint, &f);
    block_pin(0); /* the transcript stays scrollable for context while answering */
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            return finish(&f, 0);
        }

        int asking = f.focus < b->n;
        int on_option = asking && f.opt >= 0;
        int on_field = f.opt < 0;

        if (ev.key == TK_CHAR && ev.cp == KEY_CTRL('N')) {
            *how = ASKFORM_NEW_TAB;
            return finish(&f, 0);
        }
        if (ev.key == TK_ESCAPE || ev.key == TK_EOF ||
            (ev.key == TK_CHAR && (ev.cp == KEY_CTRL('C') || ev.cp == KEY_CTRL('D'))))
            return finish(&f, 0);

        int scroll = ev.key == TK_PAGE_UP || ev.key == TK_PAGE_DOWN ||
                     ev.key == TK_SCROLL_UP || ev.key == TK_SCROLL_DOWN;
        if (!scroll && viewport_scrolled())
            viewport_scroll_end();

        switch (ev.key) {
        case TK_ENTER:
            if (on_option)
                f.choice[f.focus] = f.opt;
            if (!asking || f.focus == b->n - 1)
                return finish(&f, 1);
            enter_item(&f, f.focus + 1);
            break;

        case TK_UP:
            up(&f);
            break;

        case TK_DOWN:
        case TK_TAB:
            down(&f);
            break;

        case TK_PREV_TAB:
        case TK_NEXT_TAB:
            *how = ev.key == TK_NEXT_TAB ? ASKFORM_NEXT_TAB : ASKFORM_PREV_TAB;
            return finish(&f, 0);

        case TK_PAGE_UP:
            viewport_scroll(tty_rows() / 2);
            break;

        case TK_PAGE_DOWN:
            viewport_scroll(-(tty_rows() / 2));
            break;

        case TK_SCROLL_UP:
            viewport_scroll(3);
            break;

        case TK_SCROLL_DOWN:
            viewport_scroll(-3);
            break;

        default:
            if (on_option && ev.key == TK_CHAR && ev.cp == ' ') {
                f.choice[f.focus] = f.choice[f.focus] == f.opt ? -1 : f.opt;
            } else if (on_option && typing(&ev)) {
                f.opt = -1;
                replbox_key(&f.box[f.focus], &ev);
            } else if (on_field) {
                replbox_key(&f.box[f.focus], &ev);
            }
            free(ev.text);
            break;
        }
        chrome_paint();
    }
}
