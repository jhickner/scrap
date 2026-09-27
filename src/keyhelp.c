#include "keyhelp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "tty.h"
#include "ui.h"
#include "workspace.h"

#define POLL_MS    250
#define GROUPS_MAX 16
#define LINES_MAX  128
#define KEY_GAP    2
#define COL_GAP    3
#define INDENT     1

const char KEYHELP_FOOT_ALL[] = "? or F1 \xc2\xb7 any key closes";
const char KEYHELP_FOOT_F1[]  = "F1 \xc2\xb7 any key closes";

enum line_kind { LINE_BLANK, LINE_HEAD, LINE_ROW };

struct line {
    enum line_kind kind;
    int            at;
};

struct help {
    const char               *title;
    const struct keyhelp_row *rows;
    int                       n;
    const char               *foot;
};

static int groups_of(const struct keyhelp_row *rows, int n, int *start)
{
    int g = 0;
    for (int i = 0; i < n && g < GROUPS_MAX; i++)
        if (i == 0 || strcmp(rows[i].group, rows[i - 1].group))
            start[g++] = i;
    start[g] = n;
    return g;
}

static int lines_between(const int *start, int a, int b)
{
    if (a >= b)
        return 0;
    return start[b] - start[a] + (b - a) * 2 - 1;
}

static int key_width(const struct keyhelp_row *rows, int n)
{
    size_t w = 0;
    for (int i = 0; i < n; i++)
        if (ui_cells(rows[i].key) > w)
            w = ui_cells(rows[i].key);
    return (int)w + KEY_GAP;
}

int keyhelp_layout(const struct keyhelp_row *rows, int n, int width,
                   int *split, int *colw)
{
    int start[GROUPS_MAX + 1];
    int ng = groups_of(rows, n, start);
    int key_w = key_width(rows, n);

    size_t w = 0;
    for (int i = 0; i < n; i++) {
        size_t row = (size_t)key_w + ui_cells(rows[i].brief);
        size_t head = ui_cells(rows[i].group);
        if (row > w)
            w = row;
        if (head > w)
            w = head;
    }
    *colw = (int)w;

    if (ng < 2 || 2 * *colw + COL_GAP > width) {
        *split = ng;
        return lines_between(start, 0, ng);
    }

    int best = ng, tallest = lines_between(start, 0, ng);
    for (int g = 1; g < ng; g++) {
        int left = lines_between(start, 0, g), right = lines_between(start, g, ng);
        int high = left > right ? left : right;
        if (high < tallest) {
            tallest = high;
            best = g;
        }
    }
    *split = best;
    return tallest;
}

static int column_lines(const int *start, int a, int b, struct line *out)
{
    int m = 0;
    for (int g = a; g < b && m < LINES_MAX; g++) {
        if (g > a && m < LINES_MAX)
            out[m++] = (struct line){LINE_BLANK, 0};
        if (m < LINES_MAX)
            out[m++] = (struct line){LINE_HEAD, start[g]};
        for (int i = start[g]; i < start[g + 1] && m < LINES_MAX; i++)
            out[m++] = (struct line){LINE_ROW, i};
    }
    return m;
}

static void put_fit(const char *s, int budget)
{
    if (budget <= 0)
        return;
    size_t fit = ui_fit_bytes(s, (size_t)budget);
    ui_putn(s, fit);
    ui_pad(budget - (int)ui_cells_n(s, fit));
}

static void put_cell(const struct help *h, const struct line *l, int key_w, int width)
{
    if (!l || l->kind == LINE_BLANK) {
        ui_pad(width);
        return;
    }
    const struct keyhelp_row *r = &h->rows[l->at];
    if (l->kind == LINE_HEAD) {
        ui_esc(ui_style(UI_BOLD));
        put_fit(r->group, width);
        ui_esc(ui_style(UI_RESET));
        return;
    }
    int kw = key_w < width ? key_w : width;
    ui_esc(ui_style(UI_ACCENT));
    put_fit(r->key, kw);
    ui_esc(ui_style(UI_RESET));
    put_fit(r->brief, width - kw);
}

static void border(const char *left, const char *label, const char *right, int inner)
{
    ui_pad(INDENT);
    ui_esc(ui_style(UI_DIM));
    ui_put(left);
    ui_put("\xe2\x94\x80 ");
    int used = 2;
    if (label && *label) {
        int room = inner + 2 - used - 1;
        size_t fit = ui_fit_bytes(label, room > 0 ? (size_t)room : 0);
        ui_putn(label, fit);
        ui_put(" ");
        used += (int)ui_cells_n(label, fit) + 1;
    }
    for (int i = used; i < inner + 2; i++)
        ui_put("\xe2\x94\x80");
    ui_put(right);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void paint(void *ud)
{
    const struct help *h = ud;
    int columns = ui_columns();
    int room = columns - INDENT - 4;
    if (room < 10)
        room = 10;

    int split, colw;
    keyhelp_layout(h->rows, h->n, room, &split, &colw);
    int start[GROUPS_MAX + 1];
    int ng = groups_of(h->rows, h->n, start);
    int two = split < ng;

    struct line left[LINES_MAX], right[LINES_MAX];
    int nl = column_lines(start, 0, split, left);
    int nr = two ? column_lines(start, split, ng, right) : 0;

    char title[256];
    snprintf(title, sizeof title, "keys \xc2\xb7 %s", h->title);

    int inner = two ? 2 * colw + COL_GAP : colw;
    int label = (int)ui_cells(title) > (int)ui_cells(h->foot) ? (int)ui_cells(title)
                                                             : (int)ui_cells(h->foot);
    if (inner < label + 2)
        inner = label + 2;
    if (inner > room)
        inner = room;
    int cw = two ? colw : inner;
    if (cw > inner)
        cw = inner;
    int key_w = key_width(h->rows, h->n);
    border("\xe2\x94\x8c", title, "\xe2\x94\x90", inner);

    int body = nl > nr ? nl : nr;
    int limit = chrome_modal_rows() - 2;
    if (limit < 1)
        limit = 1;
    for (int i = 0; i < body && i < limit; i++) {
        ui_pad(INDENT);
        ui_esc(ui_style(UI_DIM));
        ui_put("\xe2\x94\x82 ");
        ui_esc(ui_style(UI_RESET));
        if (i == limit - 1 && body > limit) {
            put_fit("\xe2\x80\xa6", inner);
        } else {
            put_cell(h, i < nl ? &left[i] : NULL, key_w, cw);
            if (two) {
                ui_pad(COL_GAP);
                put_cell(h, i < nr ? &right[i] : NULL, key_w, inner - cw - COL_GAP);
            }
        }
        ui_esc(ui_style(UI_DIM));
        ui_put(" \xe2\x94\x82");
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
    }
    border("\xe2\x94\x94", h->foot, "\xe2\x94\x98", inner);
}

void keyhelp_show(const char *title, const struct keyhelp_row *rows, int n,
                  const char *foot)
{
    if (n <= 0 || !frontend_has_keyboard() || !tty_is_raw())
        return;

    struct help h = {title, rows, n, foot};
    chrome_modal(paint, &h);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, POLL_MS)) {
            if (chrome_modal_interrupted())
                break;
            workspace_pump_quiet();
            continue;
        }
        free(ev.text);
        if (ev.key == TK_RESIZE) {
            chrome_paint();
            continue;
        }
        if (ev.key == TK_NONE || ev.key == TK_FOCUS_IN || ev.key == TK_FOCUS_OUT)
            continue;
        break;
    }
    chrome_modal(NULL, NULL);
}
