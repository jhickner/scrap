#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define SGR_MAX 64

/* the colour a row was left in where the box ends, so the tail resumes in it */
static void carry_sgr(const char *line, size_t n, char *out, size_t size)
{
    out[0] = '\0';
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t           end = ui_esc_span(line, n, i, &kind);
        if (end <= i)
            break;
        if (kind == UI_ESC_SGR && end - i < size)
            snprintf(out, size, "%.*s", (int)(end - i), line + i);
        i = end;
    }
}

static char *row_text(const struct overlay *o, int at)
{
    ui_sink_begin();
    o->paint_row(o->ud, at, o->w);
    return ui_sink_end();
}

static void put_over(const char *line, size_t n, const struct overlay *o, int at)
{
    char *box = row_text(o, at);

    size_t head = ui_fit_visible(line, n, (size_t)o->col);
    size_t worn = ui_cells_visible(line, head);
    ui_putn(line, head);
    if (worn < (size_t)o->col)
        ui_pad(o->col - (int)worn);

    ui_put(box ? box : "");
    free(box);

    size_t over = (size_t)(o->col + o->w);
    size_t cut = ui_fit_visible(line, n, over);
    size_t cells = ui_cells_visible(line, cut);
    /* a wide glyph straddling the right edge of the box goes under it */
    if (cells < over) {
        size_t wide = ui_fit_visible(line, n, over + 1);
        if (wide > cut) {
            cut = wide;
            cells = ui_cells_visible(line, cut);
        }
    }
    if (cells > over)
        ui_pad((int)(cells - over));

    if (cut >= n)
        return;

    char sgr[SGR_MAX];
    carry_sgr(line, cut, sgr, sizeof sgr);
    ui_esc(sgr);
    ui_putn(line + cut, n - cut);
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
        /* a raw terminal was written to with \r\n, and the return is not
           part of the row the box stands on */
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

    /* the block ran out from under the box */
    for (int line = at - o->row; line >= 0 && line < o->rows; line++) {
        char *box = row_text(o, line);
        ui_pad(o->col);
        ui_put(box ? box : "");
        free(box);
        ui_put("\n");
        at++;
    }
}
