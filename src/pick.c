#include "pick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "menu.h"
#include "overlay.h"
#include "paste.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define HIT_MAX 128

#define KEY_CTRL(c) ((c) - 'A' + 1)

struct view {
    int top;
    int visible;
    const char *title;
    const struct pick_item *items;
    const struct pick_live *live;
    const unsigned char *heading;
    int frame;
    double frame_at;
    int n;
    int *order;
    int *score;
    int count;
    int sel;
    int filter;
    int slash;
    int searching;
    char query[64];
    short hit[HIT_MAX];
};

/* the indent a menu box hangs at, under the label of the row it belongs to */
#define MENU_INDENT 6

static struct menu *open_menu(const struct view *v)
{
    struct menu *m = v->live ? v->live->menu : NULL;
    return m && m->open && m->n ? m : NULL;
}

static int item_heading(const struct view *v, int i)
{
    return v->heading &&
           (v->heading[i] == PICK_HEADING || v->heading[i] == PICK_TEXT);
}

static int item_text(const struct view *v, int i)
{
    return v->heading && v->heading[i] == PICK_TEXT;
}

static int item_group(const struct view *v, int i)
{
    return v->heading && v->heading[i] == PICK_HEADING;
}

static int item_apart(const struct view *v, int i)
{
    return v->heading && v->heading[i] == PICK_APART;
}

#define LABEL_SHARE(cols) ((cols) * 3 / 5)

static size_t align_width(const struct view *v, int columns)
{
    if (!v->live || !v->live->align)
        return 0;

    size_t width = 0;
    for (int row = 0; row < v->count; row++) {
        int i = v->order[row];
        if (item_heading(v, i) || !v->items[i].detail || !*v->items[i].detail)
            continue;
        size_t cells = ui_cells(v->items[i].label);
        if (cells > width)
            width = cells;
    }
    size_t cap = (size_t)LABEL_SHARE(columns);
    return width > cap ? cap : width;
}

/* the ellipsis has to sit inside the budget: a label that spends the whole
   budget and then trails a mark pushes its detail a cell right of the rest. */
static size_t fit_bytes(const char *s, size_t budget, int *cut)
{
    size_t fit = ui_fit_bytes(s, budget);
    *cut = s[fit] != '\0';
    if (*cut)
        fit = ui_fit_bytes(s, budget ? budget - 1 : 0);
    return fit;
}

static int item_spins(const struct view *v, int i)
{
    return v->live && v->live->spin && v->live->spin[i];
}

static int animating(const struct view *v)
{
    for (int i = 0; i < v->count; i++)
        if (item_spins(v, v->order[i]))
            return 1;
    return 0;
}

static int row_heading(const struct view *v, int row)
{
    return item_heading(v, v->order[row]);
}

static int row_group(const struct view *v, int row)
{
    return item_group(v, v->order[row]);
}

static int row_apart(const struct view *v, int row)
{
    return item_apart(v, v->order[row]);
}

static void settle(struct view *v, int dir)
{
    for (int i = 0; i < v->count; i++) {
        if (!row_heading(v, v->sel))
            return;
        v->sel += dir;
        if (v->sel >= v->count)
            v->sel = 0;
        else if (v->sel < 0)
            v->sel = v->count - 1;
    }
}

static void step(struct view *v, int dir)
{
    if (!v->count)
        return;
    v->sel += dir;
    if (v->sel >= v->count)
        v->sel = 0;
    else if (v->sel < 0)
        v->sel = v->count - 1;
    settle(v, dir);
}

static int visible_cap(const struct view *v)
{
    int rows = chrome_modal_rows() - 1;
    if (v->live)
        rows -= chrome_foot_rows(v->live->ask, v->live->hint, ui_columns());
    if (v->heading) {
        int breaks = 0;
        for (int i = 0; i < v->count; i++)
            if (row_group(v, i) || row_apart(v, i))
                breaks++;
        if (breaks > 1)
            rows -= breaks - 1;
    }
    return rows < 5 ? 5 : rows;
}

static void refilter(struct view *v)
{
    int keep = (v->count && v->sel < v->count) ? v->order[v->sel] : -1;
    int under = 0;

    v->count = 0;
    for (int i = 0; i < v->n; i++) {
        if (item_group(v, i)) {
            under = !v->query[0] || text_fuzzy_score(v->items[i].label, v->query) >= 0;
            int has = under;
            for (int j = i + 1; !has && j < v->n && !item_group(v, j); j++)
                if (text_fuzzy_score(v->items[j].label, v->query) >= 0)
                    has = 1;
            if (!has)
                continue;
            v->order[v->count] = i;
            v->score[v->count++] = 0;
            continue;
        }

        if (item_heading(v, i)) {
            if (!under)
                continue;
            v->order[v->count] = i;
            v->score[v->count++] = 0;
            continue;
        }
        int s = v->query[0] ? text_fuzzy_score(v->items[i].label, v->query) : 0;
        if (s < 0 && !under)
            continue;
        int at = v->count++;

        while (!v->heading && at > 0 && v->score[at - 1] < s) {
            v->order[at] = v->order[at - 1];
            v->score[at] = v->score[at - 1];
            at--;
        }
        v->order[at] = i;
        v->score[at] = s < 0 ? 0 : s;
    }

    v->sel = 0;
    for (int i = 0; i < v->count; i++)
        if (v->order[i] == keep) {
            v->sel = i;
            break;
        }
    if (v->count)
        settle(v, 1);
    v->top = 0;
    int cap = visible_cap(v);
    v->visible = v->count < cap ? v->count : cap;
    if (v->visible < 1)
        v->visible = 1;
}

static int run(const char *title, const struct pick_item *items, int count,
               int initial, const struct pick_live *live, const char *shortcuts,
               int *pressed, int filter, int slash);

static void paint_under(struct view *v, struct menu *box, int *box_row);

static void paint(void *ud)
{
    struct view *v = ud;
    struct menu *box = open_menu(v);
    if (!box) {
        paint_under(v, NULL, NULL);
        return;
    }

    int width = menu_width(box);
    int room = ui_columns() - MENU_INDENT - 1;
    if (width > room)
        width = room;

    int   row = 0;
    ui_sink_begin();
    paint_under(v, box, &row);
    char *under = ui_sink_end();

    struct overlay o = menu_overlay(box, row, MENU_INDENT, width);
    overlay_put(under, &o);
    free(under);
}

static void paint_under(struct view *v, struct menu *box, int *box_row)
{
    const struct pick_item *items = v->items;
    int count = v->count, sel = v->sel;
    char title[192];

    if (v->filter && (v->searching || v->query[0])) {
        const char *rest = strstr(v->title, " \xc2\xb7 ");
        int lead = rest ? (int)(rest - v->title) : (int)strlen(v->title);
        snprintf(title, sizeof title, "%.*s \xc2\xb7 /%s", lead, v->title, v->query);
    }
    else if (v->filter && !v->slash)
        snprintf(title, sizeof title, "%s \xc2\xb7 type to filter", v->title);
    else
        snprintf(title, sizeof title, "%s", v->title);

    if (sel < v->top)
        v->top = sel;
    if (sel >= v->top + v->visible)
        v->top = sel - v->visible + 1;
    if (v->top < 0)
        v->top = 0;

    if (box) {
        int under = sel + menu_rows(box);
        if (under >= v->top + v->visible)
            v->top = under - v->visible + 1;
        if (v->top > sel)
            v->top = sel;
        if (v->top < 0)
            v->top = 0;
    }

    int    columns = ui_columns();
    int    rows = 0;
    int    base = chrome_gap();
    size_t pad_to = align_width(v, columns);

    for (int i = 0; i < HIT_MAX; i++)
        v->hit[i] = -1;

    chrome_title_paint(title);
    rows++;

    int end = v->top + v->visible;
    if (end > count)
        end = count;
    for (int row = v->top; row < end; row++) {
        int i = v->order ? v->order[row] : row;
        if (item_heading(v, i)) {
            if (item_group(v, i) && row > v->top) {
                ui_put("\n");
                rows++;
            }
            ui_esc(ui_style(UI_DIM));
            ui_put("  ");
            size_t budget = columns > 3 ? (size_t)(columns - 3) : 1;
            int    cut = 0;
            size_t fit;
            if (item_text(v, i) && pad_to) {
                size_t mark = (v->live && (v->live->spin || v->live->mark)) ? 2 : 0;
                size_t cap = pad_to + 2 + mark;
                if (budget > cap)
                    budget = cap;
                fit = fit_bytes(items[i].label, budget, &cut);
            } else {
                fit = ui_fit_bytes(items[i].label, budget);
                cut = items[i].label[fit] != '\0';
            }
            ui_putn(items[i].label, fit);
            if (cut)
                ui_put("…");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            rows++;
            continue;
        }

        if (row_apart(v, row) && row > v->top) {
            ui_put("\n");
            rows++;
        }
        if (base + rows >= 0 && base + rows < HIT_MAX)
            v->hit[base + rows] = (short)row;

        int selected = (row == sel);
        ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
        ui_put(selected ? "  \xe2\x86\x92 " : "    ");

        size_t status = 0;
        if (v->live && (v->live->spin || v->live->mark)) {
            const char *mark = v->live->mark ? v->live->mark[i] : NULL;
            if (item_spins(v, i)) {
                ui_esc(ui_style(UI_SPIN));
                ui_put(spin_glyph(v->frame));
                ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
                ui_put(" ");
            } else if (mark && *mark) {
                ui_esc(ui_style(v->live->mark_role
                                ? (enum ui_role)v->live->mark_role[i]
                                : UI_ERROR));
                ui_put(mark);
                ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
                ui_put(" ");
            } else {
                ui_put("  ");
            }
            status = 2;
        }

        if (v->live && v->live->icon) {
            const char *icon = v->live->icon[i];
            if (icon && *icon) {
                ui_esc(ui_style(v->live->icon_role
                                ? (enum ui_role)v->live->icon_role[i]
                                : UI_ACCENT));
                ui_put(icon);
                ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
                ui_put(" ");
            } else {
                ui_put("  ");
            }
            status += 2;
        }

        size_t label_budget = columns > 5 + (int)status ? (size_t)(columns - 5 - (int)status) : 1;

        if (pad_to && items[i].detail && *items[i].detail && label_budget > pad_to)
            label_budget = pad_to;
        int    cut = 0;
        size_t label_n = fit_bytes(items[i].label, label_budget, &cut);
        ui_putn(items[i].label, label_n);
        if (cut)
            ui_put("…");
        ui_esc(ui_style(UI_RESET));

        size_t shown = ui_cells_n(items[i].label, label_n) + (cut ? 1 : 0);
        if (pad_to && items[i].detail && *items[i].detail && shown < pad_to) {
            ui_pad((int)(pad_to - shown));
            shown = pad_to;
        }

        size_t used = 4 + status + shown;

        if (items[i].detail && *items[i].detail) {
            int budget = columns - (int)used - 4;
            if (budget > 6) {
                ui_put("  ");
                ui_esc(ui_style(UI_DIM));
                size_t skip = 0;
                size_t fit = ui_wrap_row(items[i].detail, strlen(items[i].detail),
                                         (size_t)budget, &skip, NULL);
                ui_putn(items[i].detail, fit);
                if (items[i].detail[fit])
                    ui_put("…");
                ui_esc(ui_style(UI_RESET));
            }
        }
        ui_put("\n");
        rows++;
        if (selected && box_row)
            *box_row = rows;
    }

    if (!count) {
        ui_esc(ui_style(UI_DIM));
        ui_put("    no match");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        rows++;
    }

    if (count > v->visible) {
        ui_esc(ui_style(UI_DIM));
        ui_printf("    %d\u2013%d of %d", v->top + 1, end, count);
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        rows++;
    }

    if (v->live)
        chrome_foot_paint(v->live->ask, v->live->hint, columns);

    (void)rows;
}

int pick_run(const char *title, const struct pick_item *items, int count, int initial)
{
    return run(title, items, count, initial, NULL, NULL, NULL, 0, 0);
}

int pick_run_filter(const char *title, const struct pick_item *items, int count, int initial)
{
    return run(title, items, count, initial, NULL, NULL, NULL, 1, 0);
}

int pick_run_live(const char *title, const struct pick_item *items, int count,
                  int initial, const struct pick_live *live,
                  enum pick_search search, const char *shortcuts, int *pressed)
{
    return run(title, items, count, initial, live, shortcuts, pressed, 1,
               search == PICK_SEARCH_SLASH);
}

int pick_run_keys(const char *title, const struct pick_item *items, int count,
                  int initial, const char *shortcuts, int *pressed)
{
    return run(title, items, count, initial, NULL, shortcuts, pressed, 0, 0);
}

static int type_into(struct view *v, const char *s, size_t n)
{
    size_t len = strlen(v->query);
    int    took = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || len + 1 >= sizeof v->query)
            continue;
        v->query[len++] = (char)c;
        took = 1;
    }
    v->query[len] = '\0';
    return took;
}

static int paste_into(struct view *v, const char *s, size_t n)
{
    if (!type_into(v, s, n))
        return 0;
    v->searching = 1;
    return 1;
}

static int run(const char *title, const struct pick_item *items, int count,
               int initial, const struct pick_live *live, const char *shortcuts,
               int *pressed, int filter, int slash)
{
    if (pressed)
        *pressed = 0;
    if (count <= 0)
        return -1;

    if (!frontend_has_keyboard() || !tty_is_raw())
        return -1;

    struct view v = {0};
    v.title = title;
    v.items = items;
    v.live = live;
    v.heading = live ? live->heading : NULL;
    v.n = count;
    v.filter = filter;
    v.slash = slash;
    v.order = calloc((size_t)count, sizeof *v.order);
    v.score = calloc((size_t)count, sizeof *v.score);
    if (!v.order || !v.score) {
        free(v.order);
        free(v.score);
        return -1;
    }
    refilter(&v);
    for (int i = 0; i < v.count; i++)
        if (v.order[i] == initial) {
            v.sel = i;
            break;
        }
    if (v.count)
        settle(&v, 1);
    chrome_modal(paint, &v);

    int result = -1;
    for (;;) {
        tty_event ev;
        int turning = animating(&v);

        int asking = live && live->ask && *live->ask;
        int watching = !asking && v.live && v.live->tick;
        int wait = turning ? SPIN_FRAME_MS : (watching ? PICK_POLL_MS : -1);
        if (!tty_read(&ev, wait)) {
            if (chrome_modal_interrupted())
                goto done;
            if (!turning && !watching)
                continue;

            int moved = watching ? v.live->tick(v.live->ud) : 0;
            if (moved == PICK_TICK_REOPEN) {
                result = PICK_REOPEN;
                goto done;
            }
            if (moved)
                refilter(&v);
            if ((turning && spin_advance(&v.frame, &v.frame_at)) || moved)
                chrome_paint();
            continue;
        }
        if (asking) {
            if (ev.key == TK_TEXT) {
                free(ev.text);
                continue;
            }
            int yes = ev.key == TK_CHAR && (ev.cp == 'y' || ev.cp == 'Y');
            int no = (ev.key == TK_CHAR &&
                      (ev.cp == 'n' || ev.cp == 'N' || ev.cp == 3 || ev.cp == 4)) ||
                     ev.key == TK_ESCAPE || ev.key == TK_EOF;
            if (!yes && !no) {
                if (ev.key == TK_RESIZE) {
                    refilter(&v);
                    chrome_paint();
                }
                continue;
            }
            result = (v.count && v.sel < v.count) ? v.order[v.sel] : -1;
            if (pressed)
                *pressed = yes ? 'y' : 'n';
            goto done;
        }

        struct menu *m = open_menu(&v);
        if (m) {
            if (ev.key == TK_TEXT) {
                free(ev.text);
                continue;
            }
            if (ev.key == TK_UP || ev.key == TK_DOWN) {
                menu_step(m, ev.key == TK_UP ? -1 : 1);
                chrome_paint();
                continue;
            }
            if (ev.key == TK_LEFT || ev.key == TK_RIGHT) {
                if (menu_steer(m, ev.key == TK_LEFT ? -1 : 1))
                    chrome_paint();
                continue;
            }
            if (ev.key == TK_ENTER) {
                result = v.order[v.sel];
                if (pressed)
                    *pressed = PICK_KEY_MENU;
                goto done;
            }
            if (ev.key == TK_ESCAPE ||
                (ev.key == TK_CHAR && (ev.cp == 3 || ev.cp == 4))) {
                m->open = 0;
                refilter(&v);
                chrome_paint();
                continue;
            }
            if (ev.key == TK_EOF)
                goto done;
            if (ev.key == TK_RESIZE) {
                refilter(&v);
                chrome_paint();
            }
            continue;
        }

        int typing = filter && (!slash || v.searching);
        if (ev.key == TK_TEXT) {
            int took = filter && paste_into(&v, ev.text, ev.text ? strlen(ev.text) : 0);
            free(ev.text);
            if (!took)
                continue;
            refilter(&v);
            chrome_paint();
            continue;
        }
        switch (ev.key) {
        case TK_UP:
            step(&v, -1);
            break;
        case TK_DOWN:
            step(&v, 1);
            break;
        case TK_HOME:
            if (!v.count)
                break;
            v.sel = 0;
            settle(&v, 1);
            break;
        case TK_END:
            if (!v.count)
                break;
            v.sel = v.count - 1;
            settle(&v, -1);
            break;
        case TK_BACKSPACE: {
            if (!typing)
                break;
            if (!v.query[0]) {
                v.searching = 0;
                break;
            }
            size_t len = strlen(v.query);
            v.query[len - 1] = '\0';
            refilter(&v);
            break;
        }
        case TK_RIGHT:

            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            if (pressed && shortcuts && strchr(shortcuts, PICK_KEY_RIGHT))
                *pressed = PICK_KEY_RIGHT;
            else if (pressed && v.live && v.live->menu)
                *pressed = PICK_KEY_MENU;
            goto done;
        case TK_TAB:
            if (!shortcuts || !strchr(shortcuts, '\t'))
                break;
            if (pressed)
                *pressed = '\t';
            goto done;
        case TK_NEWLINE:

            if (!shortcuts || !strchr(shortcuts, '\n'))
                continue;
            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            if (pressed)
                *pressed = '\n';
            goto done;
        case TK_ENTER:
            if (!v.count || row_heading(&v, v.sel))
                break;
            result = v.order[v.sel];
            if (pressed && v.live && v.live->menu)
                *pressed = PICK_KEY_MENU;
            goto done;
        case TK_ESCAPE:
            if (filter && (v.searching || v.query[0])) {
                v.query[0] = '\0';
                v.searching = 0;
                refilter(&v);
                break;
            }
            goto done;
        case TK_EOF:
            goto done;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4)
                goto done;
            if (filter && ev.cp == 21) {
                v.query[0] = '\0';
                refilter(&v);
                break;
            }
            if (filter && slash && !v.searching && ev.cp == '/') {
                v.searching = 1;
                break;
            }
            if (filter && ev.cp == KEY_CTRL('V')) {
                char *text = paste_text();
                if (!text)
                    break;
                if (paste_into(&v, text, strlen(text)))
                    refilter(&v);
                free(text);
                break;
            }

            if (shortcuts && ev.cp > 0 && ev.cp < 128 && !(typing && ev.cp >= 0x20) &&
                strchr(shortcuts, (int)ev.cp)) {
                if (!v.count || row_heading(&v, v.sel))
                    break;
                result = v.order[v.sel];
                if (pressed)
                    *pressed = (int)ev.cp;
                goto done;
            }

            if (typing) {
                char c = (char)ev.cp;
                if (ev.cp < 128 && type_into(&v, &c, 1))
                    refilter(&v);
                break;
            }
            if (ev.cp >= '1' && ev.cp <= '9' && (int)(ev.cp - '1') < v.count) {
                v.sel = (int)(ev.cp - '1');
                settle(&v, 1);
            }
            break;
        case TK_MOUSE_DOWN: {
            int top = viewport_chrome_top();
            if (top < 0)
                continue;
            int at = ev.row - 1 - top;
            if (at < 0 || at >= HIT_MAX)
                continue;
            int row = v.hit[at];
            if (row < 0 || row >= v.count || row_heading(&v, row))
                continue;
            v.sel = row;
            break;
        }
        case TK_RESIZE:
            refilter(&v);
            break;
        default:
            continue;
        }
        chrome_paint();
    }

done:
    if (live && live->cursor)
        *live->cursor = (v.count && v.sel < v.count) ? v.order[v.sel] : -1;
    if (result == PICK_REOPEN || (live && live->keep))
        chrome_modal_keep();
    else
        chrome_modal(NULL, NULL);
    free(v.order);
    free(v.score);
    return result;
}
