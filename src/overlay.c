#include "overlay.h"

#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define SGR_MAX 64

struct edge {
    size_t at;
    size_t cells;
    size_t sgr;
    size_t sgr_n;
};

static void measure(const char *line, size_t n, size_t col, size_t over,
                    struct edge *head, struct edge *cut, struct edge *wide)
{
    struct edge at = {0, 0, 0, 0};

    *head = *cut = *wide = at;
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t           end = ui_esc_span(line, n, i, &kind);
        if (end <= i)
            break;
        if (kind == UI_ESC_SGR && end - i < SGR_MAX) {
            at.sgr = i;
            at.sgr_n = end - i;
        }
        size_t cells = at.cells + (kind == UI_ESC_TEXT ? ui_cells_n(line + i, end - i) : 0);
        if (cells > over + 1)
            break;
        at.at = end;
        at.cells = cells;
        if (cells <= col)
            *head = at;
        if (cells <= over)
            *cut = at;
        *wide = at;
        i = end;
    }
}

static void put_over(const char *line, size_t n, const struct overlay *o, int at)
{
    size_t col = (size_t)o->col;
    size_t over = col + (size_t)o->w;

    struct edge head, cut, wide;
    measure(line, n, col, over, &head, &cut, &wide);

    ui_putn(line, head.at);
    if (head.cells < col)
        ui_pad((int)(col - head.cells));

    o->paint_row(o->ud, at, o->w);

    struct edge tail = cut.cells < over && wide.at > cut.at ? wide : cut;
    if (tail.cells > over)
        ui_pad((int)(tail.cells - over));

    if (tail.at >= n)
        return;

    if (tail.sgr_n) {
        char sgr[SGR_MAX];
        memcpy(sgr, line + tail.sgr, tail.sgr_n);
        sgr[tail.sgr_n] = '\0';
        ui_esc(sgr);
    }
    ui_putn(line + tail.at, n - tail.at);
}

void overlay_put(const char *under, const struct overlay *o)
{
    if (!under)
        return;
    if (!o || !o->paint_row || o->rows < 1 || o->w < 1) {
        ui_put(under);
        return;
    }

    int at = 0;
    for (const char *p = under; *p;) {
        const char *nl = strchr(p, '\n');
        size_t      n = nl ? (size_t)(nl - p) : strlen(p);
        size_t      full = n;

        while (n && p[n - 1] == '\r')
            n--;

        if (at >= o->row && at < o->row + o->rows)
            put_over(p, n, o, at - o->row);
        else
            ui_putn(p, n);
        if (nl)
            ui_put("\n");

        at++;
        p += nl ? full + 1 : full;
    }

    for (int line = at - o->row; line >= 0 && line < o->rows; line++) {
        ui_pad(o->col);
        o->paint_row(o->ud, line, o->w);
        ui_put("\n");
        at++;
    }
}
