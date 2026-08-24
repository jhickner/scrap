#include "boardgrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "pick.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define GRID_LINE_MAX  256
#define GRID_TILE_ROWS (GRID_TITLE_ROWS + GRID_SPEC_ROWS + 1)

/* the rule, the space after it, and the two cells the mark sits in */
#define GRID_INDENT 4

/* the title bar and the row of lane names */
#define GRID_HEAD 2

#define LIVE_POLL_MS 500

static const char *const SPIN[] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9",
                                   "\xe2\xa0\xb8", "\xe2\xa0\xbc", "\xe2\xa0\xb4",
                                   "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87",
                                   "\xe2\xa0\x8f"};

static int text_width(int lane_w)
{
    int w = lane_w - 1 - GRID_INDENT;
    return w < 4 ? 4 : w;
}

static int text_rows(const char *s, int budget, int max)
{
    if (!s || !*s || budget < 1 || max < 1)
        return 0;

    int         k = 0;
    size_t      n = strlen(s);
    const char *p = s;
    while (n && k < max) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, n, (size_t)budget, &skip, NULL);
        if (!row && !skip)
            break;
        k++;
        size_t used = row + skip;
        p += used;
        n -= used < n ? used : n;
    }
    return k ? k : 1;
}

static int tile_height(const struct board_tile *t, int lane_w)
{
    int budget = text_width(lane_w);
    int h = text_rows(t->title, budget, GRID_TITLE_ROWS);
    if (!h)
        h = 1;
    h += text_rows(t->spec, budget, GRID_SPEC_ROWS);
    if (t->status[0] || t->pins[0])
        h++;
    return h;
}

static int grow(struct grid_layout *g, int tiles, int lanes)
{
    if (tiles > g->tiles_cap) {
        struct grid_rect *r = realloc(g->tile, (size_t)tiles * sizeof *r);
        int              *o = realloc(g->order, (size_t)tiles * sizeof *o);
        int              *h = realloc(g->height, (size_t)tiles * sizeof *h);
        if (r)
            g->tile = r;
        if (o)
            g->order = o;
        if (h)
            g->height = h;
        if (!r || !o || !h)
            return 0;
        g->tiles_cap = tiles;
    }
    if (lanes > g->lanes_cap) {
        int *top = realloc(g->lane_top, (size_t)lanes * sizeof *top);
        int *hid = realloc(g->lane_hidden, (size_t)lanes * sizeof *hid);
        int *more = realloc(g->lane_more_row, (size_t)lanes * sizeof *more);
        if (top)
            g->lane_top = top;
        if (hid)
            g->lane_hidden = hid;
        if (more)
            g->lane_more_row = more;
        if (!top || !hid || !more)
            return 0;
        g->lanes_cap = lanes;
    }
    return 1;
}

void boardgrid_layout_free(struct grid_layout *g)
{
    free(g->tile);
    free(g->order);
    free(g->height);
    free(g->lane_top);
    free(g->lane_hidden);
    free(g->lane_more_row);
    memset(g, 0, sizeof *g);
}

static int place_lane(struct grid_layout *g, int lane, int li, int k, int top,
                      int limit)
{
    int row = 0, placed = 0;
    for (int j = top; j < k; j++) {
        int h = g->height[j];
        if (j == top && h > limit)
            h = limit;
        if (row + h > limit)
            break;

        struct grid_rect *r = &g->tile[g->tiles++];
        r->lane = lane;
        r->tile = g->order[j];
        r->row = row;
        r->col = 1 + li * g->lane_w;
        r->w = g->lane_w - 1;
        r->h = h;
        r->status_row = -1;
        row += h + 1;
        placed++;
    }
    return placed;
}

int boardgrid_layout(const struct board_tile *t, const int *lane_of, int n,
                     int lanes, int cols, int rows, int sel,
                     struct grid_layout *out)
{
    if (!t || !lane_of || !out || lanes < 1 || n < 0 || rows < 1)
        return 0;
    if (!grow(out, n < 1 ? 1 : n, lanes))
        return 0;

    int room = cols - 2;
    if (room < GRID_LANE_MIN)
        return 0;

    out->tiles = 0;
    out->lanes = lanes;
    memset(out->lane_top, 0, (size_t)lanes * sizeof *out->lane_top);
    memset(out->lane_hidden, 0, (size_t)lanes * sizeof *out->lane_hidden);
    memset(out->lane_more_row, 0, (size_t)lanes * sizeof *out->lane_more_row);
    out->rows_used = 0;

    int shown = room / GRID_LANE_MIN;
    if (shown < 1)
        shown = 1;
    if (shown > lanes)
        shown = lanes;
    out->lanes_shown = shown;
    out->lane_w = room / shown > GRID_LANE_MAX ? GRID_LANE_MAX : room / shown;

    int at = sel >= 0 && sel < n ? lane_of[sel] : 0;
    if (out->lane_first > lanes - shown)
        out->lane_first = lanes - shown;
    if (out->lane_first < 0)
        out->lane_first = 0;
    if (at < out->lane_first)
        out->lane_first = at;
    if (at >= out->lane_first + shown)
        out->lane_first = at - shown + 1;

    for (int li = 0; li < shown; li++) {
        int lane = out->lane_first + li;

        int k = 0;
        for (int i = 0; i < n; i++) {
            if (lane_of[i] != lane)
                continue;
            out->order[k] = i;
            out->height[k] = tile_height(&t[i], out->lane_w);
            k++;
        }
        if (!k)
            continue;

        int top = 0;
        if (lane == at) {
            int s = 0;
            while (s < k && out->order[s] != sel)
                s++;
            while (s < k && top < s) {
                int used = 0, fits = 0;
                for (int j = top; j <= s; j++) {
                    if (used + out->height[j] > rows)
                        break;
                    used += out->height[j] + 1;
                    fits = j == s;
                }
                if (fits)
                    break;
                top++;
            }
        }
        out->lane_top[lane] = top;

        int mark = out->tiles;
        int placed = place_lane(out, lane, li, k, top, rows);
        if (placed < k - top && rows > 1) {
            out->tiles = mark;
            placed = place_lane(out, lane, li, k, top, rows - 1);
        }
        out->lane_hidden[lane] = k - placed;
    }

    for (int i = 0; i < out->tiles; i++) {
        struct grid_rect        *r = &out->tile[i];
        const struct board_tile *tile = &t[r->tile];
        if (tile->status[0] || tile->pins[0])
            r->status_row = r->row + r->h - 1;

        int end = r->row + r->h;
        if (end > out->lane_more_row[r->lane])
            out->lane_more_row[r->lane] = end;
        if (end > out->rows_used)
            out->rows_used = end;
    }

    for (int li = 0; li < shown; li++) {
        int lane = out->lane_first + li;
        if (!out->lane_hidden[lane])
            continue;
        if (out->lane_more_row[lane] >= rows)
            out->lane_more_row[lane] = rows - 1;
        if (out->lane_more_row[lane] + 1 > out->rows_used)
            out->rows_used = out->lane_more_row[lane] + 1;
    }
    return 1;
}

int boardgrid_hit(const struct grid_layout *g, int row, int col, int *part)
{
    if (part)
        *part = GRID_PART_TILE;
    if (!g)
        return -1;

    for (int i = 0; i < g->tiles; i++) {
        const struct grid_rect *r = &g->tile[i];
        if (row < r->row || row >= r->row + r->h)
            continue;
        if (col < r->col || col >= r->col + r->w)
            continue;
        if (part && row == r->status_row)
            *part = GRID_PART_STATUS;
        return r->tile;
    }
    return -1;
}

struct lines {
    char          text[GRID_TILE_ROWS][GRID_LINE_MAX];
    unsigned char role[GRID_TILE_ROWS];
    int           n;
};

static int wrap_into(const char *s, int budget, int max, struct lines *out,
                     enum ui_role role)
{
    if (!s || !*s || max < 1)
        return 0;

    int         k = 0;
    size_t      n = strlen(s);
    const char *p = s;
    while (n && k < max && out->n < GRID_TILE_ROWS) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, n, (size_t)budget, &skip, NULL);
        if (!row && !skip)
            break;
        size_t used = row + skip;

        char *into = out->text[out->n];
        if (k == max - 1 && used < n) {
            size_t fit = ui_fit_bytes(p, budget > 1 ? (size_t)budget - 1 : 1);
            snprintf(into, GRID_LINE_MAX, "%.*s…", (int)fit, p);
        } else
            snprintf(into, GRID_LINE_MAX, "%.*s", (int)row, p);

        out->role[out->n] = (unsigned char)role;
        out->n++;
        k++;
        p += used;
        n -= used < n ? used : n;
    }
    return k;
}

static void tile_lines(const struct board_tile *t, int budget, struct lines *out)
{
    out->n = 0;
    wrap_into(t->title, budget, GRID_TITLE_ROWS, out, UI_TEXT);
    wrap_into(t->spec, budget, GRID_SPEC_ROWS, out, UI_DIM);

    if (t->status[0] || t->pins[0]) {
        char say[GRID_LINE_MAX];
        snprintf(say, sizeof say, "%s%s%s", t->status,
                 t->status[0] && t->pins[0] ? " · " : "", t->pins);
        size_t fit = ui_fit_bytes(say, (size_t)budget);
        if (say[fit])
            snprintf(out->text[out->n], GRID_LINE_MAX, "%.*s…",
                     (int)ui_fit_bytes(say, budget > 1 ? (size_t)budget - 1 : 1),
                     say);
        else
            snprintf(out->text[out->n], GRID_LINE_MAX, "%s", say);
        out->role[out->n] = UI_DIM;
        out->n++;
    }
}

struct grid {
    const char              *title;
    const struct board_tile *t;
    const int               *lane_of;
    const char *const       *lane_name;
    int                      n, lanes;
    int                      sel;
    const char              *hint;
    const char              *ask;
    int                      frame;
    double                   frame_at;
    int                      rows;
    int                      ok;
    struct grid_layout       g;
    struct lines            *drawn;
    int                      drawn_n;
};

static size_t ask_budget(int columns)
{
    return columns > 12 ? (size_t)(columns - 12) : 1;
}

static int ask_rows(const char *ask, int columns)
{
    struct ui_wrap w = {0};
    w.budget = ask_budget(columns);
    w.measure = 1;
    w.paint_empty = 1;
    return ui_wrap_paint(ask, &w);
}

static int grid_rows(const struct grid *v)
{
    int rows = tty_rows() - 3 - chrome_gap() - (GRID_HEAD - 1);
    if (v->ask && *v->ask)
        rows -= 1 + ask_rows(v->ask, ui_columns());
    else if (v->hint && *v->hint) {
        rows -= 2;
        for (const char *p = v->hint; (p = strchr(p, '\n')); p++)
            rows--;
    }
    return rows < 3 ? 3 : rows;
}

static int relayout(struct grid *v)
{
    v->rows = grid_rows(v);
    v->ok = boardgrid_layout(v->t, v->lane_of, v->n, v->lanes, ui_columns(),
                             v->rows, v->sel, &v->g);
    if (!v->ok)
        return 0;

    if (v->g.tiles > v->drawn_n) {
        struct lines *grown =
            realloc(v->drawn, (size_t)v->g.tiles * sizeof *grown);
        if (!grown)
            return 0;
        v->drawn = grown;
        v->drawn_n = v->g.tiles;
    }
    int budget = text_width(v->g.lane_w);
    for (int i = 0; i < v->g.tiles; i++)
        tile_lines(&v->t[v->g.tile[i].tile], budget, &v->drawn[i]);
    return 1;
}

static const struct grid_rect *rect_at(const struct grid *v, int lane, int row,
                                       int *at)
{
    for (int i = 0; i < v->g.tiles; i++) {
        const struct grid_rect *r = &v->g.tile[i];
        if (r->lane != lane || row < r->row || row >= r->row + r->h)
            continue;
        *at = i;
        return r;
    }
    return NULL;
}

static void put_fit(const char *s, int budget, enum ui_role role, int *used)
{
    size_t fit = ui_fit_bytes(s, (size_t)budget);
    ui_esc(ui_style(role));
    ui_putn(s, fit);
    ui_esc(ui_style(UI_RESET));
    *used += (int)ui_cells_n(s, fit);
}

static void pad_to(int *used, int col)
{
    if (col > *used) {
        ui_pad(col - *used);
        *used = col;
    }
}

static void paint_names(struct grid *v)
{
    int used = 0;
    for (int li = 0; li < v->g.lanes_shown; li++) {
        int lane = v->g.lane_first + li;
        pad_to(&used, 1 + li * v->g.lane_w + 2);
        put_fit(v->lane_name[lane], v->g.lane_w - 3, UI_HEADING, &used);
    }
    ui_put("\n");
}

static void paint_row(struct grid *v, int row)
{
    int used = 0;
    for (int li = 0; li < v->g.lanes_shown; li++) {
        int lane = v->g.lane_first + li;
        int at = -1;
        const struct grid_rect *r = rect_at(v, lane, row, &at);

        if (!r) {
            if (row == v->g.lane_more_row[lane] && v->g.lane_hidden[lane] > 0) {
                char more[16];
                snprintf(more, sizeof more, "+%d", v->g.lane_hidden[lane]);
                pad_to(&used, 1 + li * v->g.lane_w + GRID_INDENT);
                put_fit(more, v->g.lane_w - 1 - GRID_INDENT, UI_DIM, &used);
            }
            continue;
        }

        const struct board_tile *t = &v->t[r->tile];
        int                      picked = r->tile == v->sel;

        pad_to(&used, r->col);
        ui_esc(ui_style(picked ? UI_ACCENT : (enum ui_role)t->mark_role));
        ui_put(UI_BAR);
        ui_esc(ui_style(UI_RESET));
        used++;
        pad_to(&used, r->col + 2);

        if (row == r->row) {
            if (t->spin) {
                ui_esc(ui_style(UI_SPIN));
                ui_put(SPIN[v->frame % (int)(sizeof SPIN / sizeof *SPIN)]);
                ui_esc(ui_style(UI_RESET));
                used++;
            } else if (t->mark && *t->mark) {
                ui_esc(ui_style((enum ui_role)t->mark_role));
                ui_put(t->mark);
                ui_esc(ui_style(UI_RESET));
                used += (int)ui_cells(t->mark);
            }
        }
        pad_to(&used, r->col + GRID_INDENT);

        int line = row - r->row;
        if (line < v->drawn[at].n)
            put_fit(v->drawn[at].text[line], v->g.lane_w - 1 - GRID_INDENT,
                    (enum ui_role)v->drawn[at].role[line], &used);
    }
    ui_put("\n");
}

static void paint(void *ud)
{
    struct grid *v = ud;
    if (!relayout(v))
        return;

    int  columns = ui_columns();
    char title[256];
    if (v->g.lanes_shown < v->lanes)
        snprintf(title, sizeof title, "%s · lanes %d-%d of %d", v->title,
                 v->g.lane_first + 1, v->g.lane_first + v->g.lanes_shown,
                 v->lanes);
    else
        snprintf(title, sizeof title, "%s", v->title);

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        size_t budget = columns > 3 ? (size_t)(columns - 3) : 1;
        size_t fit = ui_fit_bytes(title, budget);
        ui_putn(title, fit);
        if (title[fit])
            ui_put("…");
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    paint_names(v);
    for (int row = 0; row < v->g.rows_used; row++)
        paint_row(v, row);

    if (v->ask && *v->ask) {
        const char *p = v->ask;
        size_t      n = strlen(p);
        size_t      budget = ask_budget(columns);
        ui_put("\n");
        while (n) {
            size_t skip = 0;
            size_t row = ui_wrap_row(p, n, budget, &skip, NULL);
            size_t used = row + skip;
            ui_put("    ");
            ui_putn(p, row);
            p += used;
            n -= used < n ? used : n;
            if (n)
                ui_put("\n");
        }
        ui_put(" ");
        ui_esc(ui_style(UI_ACCENT));
        ui_put("y/n");
        ui_esc(ui_style(UI_RESET));
    } else if (v->hint && *v->hint) {
        ui_put("\n");
        size_t budget = columns > 6 ? (size_t)(columns - 6) : 1;
        for (const char *p = v->hint; p;) {
            const char *nl = strchr(p, '\n');
            size_t      n = nl ? (size_t)(nl - p) : strlen(p);
            ui_esc(ui_style(UI_DIM));
            ui_put("    ");
            ui_putn(p, ui_fit_visible(p, n, budget));
            ui_esc(ui_style(UI_RESET));
            if (!nl)
                break;
            ui_put("\n");
            p = nl + 1;
        }
    }
}

static int animating(const struct grid *v)
{
    for (int i = 0; i < v->n; i++)
        if (v->t[i].spin)
            return 1;
    return 0;
}

static void step_in_lane(struct grid *v, int dir)
{
    int lane = v->lane_of[v->sel];
    int want = -1;
    for (int i = v->sel + dir; i >= 0 && i < v->n; i += dir)
        if (v->lane_of[i] == lane) {
            want = i;
            break;
        }
    if (want >= 0)
        v->sel = want;
}

static void step_lane(struct grid *v, int dir)
{
    int lane = v->lane_of[v->sel];
    int at = 0;
    for (int i = 0; i < v->sel; i++)
        at += v->lane_of[i] == lane;

    for (int want = lane + dir; want >= 0 && want < v->lanes; want += dir) {
        int k = 0, pick = -1;
        for (int i = 0; i < v->n; i++) {
            if (v->lane_of[i] != want)
                continue;
            if (k <= at)
                pick = i;
            k++;
        }
        if (pick >= 0) {
            v->sel = pick;
            return;
        }
    }
}

static int move_key(struct grid *v, tty_key key, uint32_t cp,
                    const char *shortcuts)
{
    int free_key = cp && (!shortcuts || !strchr(shortcuts, (int)cp));

    if (key == TK_UP || (free_key && cp == 'k')) {
        step_in_lane(v, -1);
        return 1;
    }
    if (key == TK_DOWN || (free_key && cp == 'j')) {
        step_in_lane(v, 1);
        return 1;
    }
    if (key == TK_LEFT || (free_key && cp == 'h')) {
        step_lane(v, -1);
        return 1;
    }
    if (key == TK_RIGHT || (free_key && cp == 'l')) {
        step_lane(v, 1);
        return 1;
    }
    return 0;
}

int boardgrid_run(const char *title, const struct board_tile *tiles,
                  const int *lane_of, const char *const *lane_name, int n,
                  int lanes, int initial, const char *hint, const char *ask,
                  const char *shortcuts, int *pressed, int (*tick)(void *ud),
                  void *tick_ud, int *cursor, int *part)
{
    if (pressed)
        *pressed = 0;
    if (part)
        *part = GRID_PART_TILE;
    if (n <= 0 || lanes <= 0)
        return -1;
    if (!frontend_has_keyboard() || !tty_is_raw())
        return -1;
    if (ui_too_narrow())
        return GRID_NARROW;

    struct grid v = {0};
    v.title = title;
    v.t = tiles;
    v.lane_of = lane_of;
    v.lane_name = lane_name;
    v.n = n;
    v.lanes = lanes;
    v.sel = initial >= 0 && initial < n ? initial : 0;
    v.hint = hint;
    v.ask = ask;

    int result = -1;
    if (!relayout(&v)) {
        result = GRID_NARROW;
        goto done;
    }
    chrome_modal(paint, &v);

    for (;;) {
        tty_event ev;
        int       turning = animating(&v);

        int asking = ask && *ask;
        int watching = !asking && tick;
        int wait = turning ? SPIN_FRAME_MS : (watching ? LIVE_POLL_MS : -1);
        if (!tty_read(&ev, wait)) {
            if (chrome_modal_interrupted())
                goto done;
            if (!turning && !watching)
                continue;

            int moved = watching ? tick(tick_ud) : 0;
            if (moved == PICK_TICK_REOPEN) {
                result = PICK_REOPEN;
                goto done;
            }
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
                if (ev.key == TK_RESIZE)
                    chrome_paint();
                continue;
            }
            result = v.sel;
            if (pressed)
                *pressed = yes ? 'y' : 'n';
            goto done;
        }

        if (ev.key == TK_TEXT) {
            free(ev.text);
            continue;
        }
        if (move_key(&v, ev.key, ev.key == TK_CHAR ? ev.cp : 0, shortcuts)) {
            chrome_paint();
            continue;
        }

        switch (ev.key) {
        case TK_TAB:
            if (!shortcuts || !strchr(shortcuts, '\t'))
                break;
            if (pressed)
                *pressed = '\t';
            goto done;
        case TK_ENTER:
            result = v.sel;
            goto done;
        case TK_ESCAPE:
        case TK_EOF:
            goto done;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4)
                goto done;
            if (shortcuts && ev.cp > 0 && ev.cp < 128 &&
                strchr(shortcuts, (int)ev.cp)) {
                result = v.sel;
                if (pressed)
                    *pressed = (int)ev.cp;
                goto done;
            }
            break;
        case TK_MOUSE_DOWN: {
            int top = viewport_chrome_top();
            if (top < 0)
                break;
            int row = ev.row - 1 - top - chrome_gap() - GRID_HEAD;
            int was = 0;
            int at = boardgrid_hit(&v.g, row, ev.col - 1, &was);
            if (at < 0)
                break;
            if (at != v.sel) {
                v.sel = at;
                break;
            }
            result = v.sel;
            if (part)
                *part = was;
            goto done;
        }
        case TK_RESIZE:
            if (!relayout(&v)) {
                result = GRID_NARROW;
                goto done;
            }
            break;
        default:
            break;
        }
        chrome_paint();
    }

done:
    if (cursor)
        *cursor = v.sel;
    chrome_modal_keep();
    boardgrid_layout_free(&v.g);
    free(v.drawn);
    return result;
}
