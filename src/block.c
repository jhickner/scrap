#include "block.h"

#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define ROWS_MAX 512

static int row_edit = -1;
static int have;
static int fill;
static int pinned;

static int split(char *body, char **line, int max)
{
    int   n = 0;
    char *s = body;
    while (n < max) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = '\0';
        size_t l = strlen(s);
        while (l && s[l - 1] == '\r')
            s[--l] = '\0';
        line[n++] = s;
        if (!nl || !nl[1])
            break;
        s = nl + 1;
    }
    return n;
}

void block_fill(int on) { fill = on ? 1 : 0; }

void block_pin(int on)
{
    pinned = on ? 1 : 0;
    viewport_chrome_pin(pinned);
}

void block_begin(void)
{
    row_edit = -1;
    ui_sink_begin();
}

void block_end(int caret_row, int caret_col)
{
    char *body = ui_sink_end();
    if (!body) {
        block_clear();
        return;
    }

    static char blank[] = "";

    char *line[ROWS_MAX];
    int   n = split(body, line, ROWS_MAX);

    int limit = tty_rows() - !fill;
    if (limit > ROWS_MAX)
        limit = ROWS_MAX;
    if (n > limit)
        n = limit > 0 ? limit : 1;
    while (fill && n < limit)
        line[n++] = blank;

    viewport_chrome(line, n, caret_row, caret_col);
    have = n > 0;
    viewport_paint();
    free(body);
}

int block_have(void) { return have; }

void block_row_begin(int row)
{
    row_edit = row;
    ui_sink_begin();
}

void block_row_end(void)
{
    char *body = ui_sink_end();
    int   at = row_edit;
    row_edit = -1;
    if (!body)
        return;

    char *line[ROWS_MAX];
    int   n = split(body, line, ROWS_MAX);
    if (at >= 0 && n > 0)
        viewport_chrome_row(at, line[0]);
    viewport_paint();
    free(body);
}

void block_clear(void)
{
    have = 0;
    viewport_chrome_clear();
    viewport_paint();
}

void block_keep(int keep)
{
    viewport_chrome_keep(keep);
    have = 0;
    viewport_paint();
}

void block_forget(void)
{
    have = 0;
    if (!pinned)
        viewport_chrome_clear();
}

void block_cleared(void)
{
    have = 0;
    if (!pinned)
        viewport_chrome_clear();
    viewport_clear();
}
