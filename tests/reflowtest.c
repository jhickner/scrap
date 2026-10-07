
#include <stdio.h>
#include <string.h>

#include "ui.h"

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    ui_init();
    expect((int)ui_cells("abc"), 3, "ascii is one cell each");
    expect((int)ui_cells("\xe4\xb8\xad"), 2, "CJK is two cells");
    expect((int)ui_cells("\xe2\x9c\x93"), 1, "check mark is one cell");
    expect((int)ui_cells("\xe2\x94\x82"), 1, "box drawing is one cell");
    expect((int)ui_cells("\xe2\x96\x8c"), 1, "the bar glyph is one cell");
    expect((int)ui_cells("\xf0\x9f\x98\x80"), 2, "emoji is two cells");
    expect((int)ui_cells("\xe2\x9c\x85"), 2, "emoji-presentation is two");

    size_t skip = 0;
    expect((int)ui_wrap_row("\xe4\xb8\xad", 3, 1, &skip, NULL), 3, "too-wide glyph is consumed");
    expect((int)skip, 0, "and reports no skip");

    char long_line[201];
    memset(long_line, 'x', 200);
    long_line[200] = '\0';
    size_t cells = 0;
    size_t first = ui_wrap_row(long_line, 200, 10, &skip, &cells);
    expect((int)first, 10, "long row uses the given length, not strlen");
    expect((int)cells, 10, "wrap reports the row's cell count");
    expect((int)skip, 0, "no space means no skip");

    struct ui_wrap w = {0};
    w.budget = 10;
    w.measure = 1;
    w.paint_empty = 1;
    expect(ui_wrap_paint("", &w), 1, "empty text is one row");
    expect(ui_wrap_paint("hello", &w), 1, "text inside the budget is one row");
    expect(ui_wrap_paint("hello world again", &w), 3, "wraps on word boundaries");
    w.max_rows = 1;
    expect(ui_wrap_paint("hello world again", &w), 1, "max_rows clips");

    int widths[4];
    w.max_rows = 0;
    w.gutter = "| ";
    w.mark = "* ";
    w.widths = widths;
    w.widths_max = 4;
    expect(ui_wrap_paint("abc", &w), 1, "one row with a gutter");
    expect(widths[0], 2 + 2 + 3, "gutter and mark counted on the first row");

    if (failures == 0)
        printf("reflowtest: all checks passed\n");
    return failures != 0;
}
