#include "boardgrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "menu.h"
#include "overlay.h"
#include "pick.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define GRID_LINE_MAX  256

/* the title, the divider under it, the status and the footer, then the
   divider over the log preview and the preview itself */
#define GRID_TILE_ROWS (GRID_TITLE_ROWS + 4 + BOARD_RECENT)

/* the left border and the space after it */
#define GRID_INDENT 2

/* the cells the spinner takes at the head of the status row */
#define GRID_SPIN 2

/* the top and bottom border rows a tile carries beyond its text */
#define GRID_BORDER 2

#define BOX_TL "\xe2\x95\xad"
#define BOX_TR "\xe2\x95\xae"
#define BOX_BL "\xe2\x95\xb0"
#define BOX_BR "\xe2\x95\xaf"
#define BOX_H  "\xe2\x94\x80"
#define BOX_V  "\xe2\x94\x82"
#define BOX_ML "\xe2\x94\x9c"
#define BOX_MR "\xe2\x94\xa4"

/* the title bar and the row of lane names */
#define GRID_HEAD 2

/* the cells a tile's text has between the mark and the right border */
static int text_width(int lane_w)
{
    int w = lane_w - 3 - GRID_INDENT;
    return w < 4 ? 4 : w;
}

static int has_status(const struct board_tile *t) { return t->status[0] != 0; }

/* the backend or tier the card was pinned to */
static int foot_of(const struct board_tile *t, char *out, size_t size)
{
    snprintf(out, size, "%s", t->pins);
    return out[0] != '\0';
}

/* the status and the pins share a row when the two fit in one */
static int foot_joined(const struct board_tile *t, int budget, char *out,
                       size_t size)
{
    char foot[GRID_LINE_MAX];
    if (!has_status(t) || !foot_of(t, foot, sizeof foot))
        return 0;
    int room = t->spin ? budget - GRID_SPIN : budget;
    if ((int)ui_cells(t->status) + 3 + (int)ui_cells(foot) > room)
        return 0;
    snprintf(out, size, "%s \xc2\xb7 %s", t->status, foot);
    return 1;
}

/* the last lines of the session log, while the card is working */
static int say_rows(const struct board_tile *t)
{
    return t->working ? t->recent_n : 0;
}

struct lines {
    char          text[GRID_TILE_ROWS][GRID_LINE_MAX];
    unsigned char role[GRID_TILE_ROWS];
    unsigned char rule[GRID_TILE_ROWS];   /* the divider under the title */
    unsigned char indent[GRID_TILE_ROWS]; /* the cells the spinner leaves */
    int           n;
    int           title_rows;
    /* the rows below the status row: what a click there is not */
    int           under;
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

static void add_line(struct lines *out, const char *s, int budget,
                     enum ui_role role, int indent)
{
    if (out->n >= GRID_TILE_ROWS)
        return;
    char *into = out->text[out->n];
    if ((int)ui_cells(s) > budget)
        snprintf(into, GRID_LINE_MAX, "%.*s…",
                 (int)ui_fit_bytes(s, budget > 1 ? (size_t)budget - 1 : 1), s);
    else
        snprintf(into, GRID_LINE_MAX, "%s", s);
    out->role[out->n] = (unsigned char)role;
    out->indent[out->n] = (unsigned char)indent;
    out->n++;
}

static void tile_lines(const struct board_tile *t, int budget, struct lines *out)
{
    memset(out, 0, sizeof *out);
    out->title_rows = wrap_into(t->title, budget, GRID_TITLE_ROWS, out, UI_TEXT);

    char foot[GRID_LINE_MAX];
    int  has_foot = foot_of(t, foot, sizeof foot);
    int  say = say_rows(t);
    if (!has_status(t) && !has_foot && !say)
        return;

    if ((has_status(t) || has_foot) && out->n < GRID_TILE_ROWS)
        out->rule[out->n++] = 1;

    char joined[GRID_LINE_MAX];
    if (foot_joined(t, budget, joined, sizeof joined)) {
        add_line(out, joined, t->spin ? budget - GRID_SPIN : budget, UI_DIM,
                 t->spin ? GRID_SPIN : 0);
        has_foot = 0;
    } else if (has_status(t))
        add_line(out, t->status, t->spin ? budget - GRID_SPIN : budget, UI_DIM,
                 t->spin ? GRID_SPIN : 0);

    int mark = out->n;
    if (has_foot)
        add_line(out, foot, budget, UI_DIM, 0);

    if (say && out->n < GRID_TILE_ROWS) {
        out->rule[out->n++] = 1;
        for (int i = 0; i < say; i++)
            add_line(out, t->recent[i], budget, UI_DIM, 0);
    }

    out->under = out->n - mark;
}

static int tile_height(const struct lines *l)
{
    return l->n + (l->title_rows ? 0 : 1) + GRID_BORDER;
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

/* the first card of a lane to draw so that the one at s is on screen */
static int scroll_top(const struct grid_layout *g, int k, int s, int limit)
{
    int top = 0;
    while (s < k && top < s) {
        int used = 0, fits = 0;
        for (int j = top; j <= s; j++) {
            if (used + g->height[j] > limit)
                break;
            used += g->height[j];
            fits = j == s;
        }
        if (fits)
            break;
        top++;
    }
    return top;
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
        row += h;
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

    /* the tile's rows are laid out once and the height read off them */
    struct tile_size { int h, under; };
    struct tile_size *size = calloc((size_t)(n < 1 ? 1 : n), sizeof *size);
    if (!size)
        return 0;

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
            struct lines l;
            tile_lines(&t[i], text_width(out->lane_w), &l);
            size[i].h = tile_height(&l);
            size[i].under = l.under;

            out->order[k] = i;
            out->height[k] = size[i].h;
            k++;
        }
        if (!k)
            continue;

        int s = -1;
        if (lane == at) {
            s = 0;
            while (s < k && out->order[s] != sel)
                s++;
        }

        int top = s >= 0 ? scroll_top(out, k, s, rows) : 0;

        int mark = out->tiles;
        int placed = place_lane(out, lane, li, k, top, rows);
        /* the count of what is hidden takes a row from the cards */
        if (placed < k - top && rows > 1) {
            out->tiles = mark;
            if (s >= 0)
                top = scroll_top(out, k, s, rows - 1);
            placed = place_lane(out, lane, li, k, top, rows - 1);
        }
        out->lane_top[lane] = top;
        out->lane_hidden[lane] = k - placed;
    }

    for (int i = 0; i < out->tiles; i++) {
        struct grid_rect        *r = &out->tile[i];
        const struct board_tile *tile = &t[r->tile];
        /* a tile clipped to a short lane has lost its status row: what sits
           where the row would be is title, and a click there is not the worker */
        if (has_status(tile) && r->h == size[r->tile].h) {
            int row = r->row + r->h - 2 - size[r->tile].under;
            if (row > r->row)
                r->status_row = row;
        }

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

    free(size);
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

struct grid {
    const char              *title;
    struct menu             *menu;
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

static int grid_rows(const struct grid *v)
{
    int rows = chrome_modal_rows() - GRID_HEAD;
    rows -= chrome_foot_rows(v->ask, v->hint, ui_columns());
    return rows < 3 ? 3 : rows;
}

static struct menu *open_menu(const struct grid *v)
{
    return v->menu && v->menu->open && v->menu->n ? v->menu : NULL;
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

/* where the box hangs against the tile the cursor is on, pulled up the lane
   when the rows under the tile have run out */
static int menu_box(const struct grid *v, int *top, int *col, int *w)
{
    struct menu *m = open_menu(v);
    if (!m)
        return -1;

    for (int i = 0; i < v->g.tiles; i++) {
        const struct grid_rect *r = &v->g.tile[i];
        if (r->tile != v->sel)
            continue;
        int rows = menu_rows(m);
        int at = r->row + r->h;
        if (at + rows > v->rows)
            at = v->rows - rows;
        if (at < 0)
            at = 0;
        *top = at;
        *col = r->col;
        *w = menu_width(m) > r->w ? r->w : menu_width(m);
        return r->lane;
    }
    return -1;
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

static void put_edge(enum ui_role role, const char *s, int *used)
{
    ui_esc(ui_style(role));
    ui_put(s);
    ui_esc(ui_style(UI_RESET));
    (*used)++;
}

static void put_divider(enum ui_role role, int w, int *used)
{
    ui_esc(ui_style(role));
    ui_put(BOX_ML);
    for (int i = 2; i < w; i++)
        ui_put(BOX_H);
    if (w > 1)
        ui_put(BOX_MR);
    ui_esc(ui_style(UI_RESET));
    *used += w;
}

static void put_rule(enum ui_role role, int top, int w, int *used)
{
    ui_esc(ui_style(role));
    ui_put(top ? BOX_TL : BOX_BL);
    for (int i = 2; i < w; i++)
        ui_put(BOX_H);
    if (w > 1)
        ui_put(top ? BOX_TR : BOX_BR);
    ui_esc(ui_style(UI_RESET));
    *used += w;
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
        enum ui_role edge = picked ? UI_ACCENT : UI_DIM;

        pad_to(&used, r->col);
        if (row == r->row || row == r->row + r->h - 1) {
            put_rule(edge, row == r->row, r->w, &used);
            continue;
        }

        int line = row - r->row - 1;
        if (line >= 0 && line < v->drawn[at].n && v->drawn[at].rule[line]) {
            put_divider(edge, r->w, &used);
            continue;
        }

        put_edge(edge, BOX_V, &used);
        pad_to(&used, r->col + GRID_INDENT);

        if (line >= 0 && line < v->drawn[at].n) {
            int indent = v->drawn[at].indent[line];
            if (indent && t->spin) {
                ui_esc(ui_style(UI_SPIN));
                ui_put(spin_glyph(v->frame));
                ui_esc(ui_style(UI_RESET));
                used++;
            }
            pad_to(&used, r->col + GRID_INDENT + indent);
            enum ui_role role = (enum ui_role)v->drawn[at].role[line];
            if (picked && line < v->drawn[at].title_rows)
                role = UI_ACCENT;
            put_fit(v->drawn[at].text[line], text_width(v->g.lane_w) - indent,
                    role, &used);
        }

        pad_to(&used, r->col + r->w - 1);
        put_edge(edge, BOX_V, &used);
    }
    ui_put("\n");
}

static void paint_under(struct grid *v)
{
    char title[256];
    if (v->g.lanes_shown < v->lanes)
        snprintf(title, sizeof title, "%s · lanes %d-%d of %d", v->title,
                 v->g.lane_first + 1, v->g.lane_first + v->g.lanes_shown,
                 v->lanes);
    else
        snprintf(title, sizeof title, "%s", v->title);

    chrome_title_paint(title);
    paint_names(v);
    /* every pass paints the whole budget, or the board walks up the screen
       as a lane scrolls and the rows it needs change */
    for (int row = 0; row < v->rows; row++)
        paint_row(v, row);

    chrome_foot_paint(v->ask, v->hint, ui_columns());
}

static void paint(void *ud)
{
    struct grid *v = ud;
    if (!relayout(v))
        return;

    int top = 0, col = 0, w = 0;
    if (menu_box(v, &top, &col, &w) < 0) {
        paint_under(v);
        return;
    }

    ui_sink_begin();
    paint_under(v);
    char *under = ui_sink_end();

    struct overlay o = menu_overlay(v->menu, GRID_HEAD + top, col, w);
    overlay_put(under, &o);
    free(under);
}

static int animating(const struct grid *v)
{
    for (int i = 0; i < v->g.tiles; i++)
        if (v->t[v->g.tile[i].tile].spin)
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
    int free_key = cp && cp < 128 &&
                   (!shortcuts || !strchr(shortcuts, (int)cp));

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
                  void *tick_ud, int *cursor, int *part, struct menu *menu)
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
    v.menu = menu;

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
        int wait = turning ? SPIN_FRAME_MS : (watching ? PICK_POLL_MS : -1);
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

        if (open_menu(&v)) {
            if (ev.key == TK_UP || ev.key == TK_DOWN) {
                menu_step(v.menu, ev.key == TK_UP ? -1 : 1);
                chrome_paint();
                continue;
            }
            if (ev.key == TK_LEFT || ev.key == TK_RIGHT) {
                if (menu_steer(v.menu, ev.key == TK_LEFT ? -1 : 1))
                    chrome_paint();
                continue;
            }
            if (ev.key == TK_ENTER) {
                result = v.sel;
                if (pressed)
                    *pressed = PICK_KEY_MENU;
                goto done;
            }
            if (ev.key == TK_ESCAPE ||
                (ev.key == TK_CHAR && (ev.cp == 3 || ev.cp == 4))) {
                v.menu->open = 0;
                if (!relayout(&v)) {
                    result = GRID_NARROW;
                    goto done;
                }
                chrome_paint();
                continue;
            }
            if (ev.key == TK_EOF)
                goto done;
            if (ev.key == TK_RESIZE && !relayout(&v)) {
                result = GRID_NARROW;
                goto done;
            }
            chrome_paint();
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
