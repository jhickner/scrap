#include "sidechannelview.h"

#include <stddef.h>

#include "ui.h"

int sidechannel_question_paint(const char *question, const char *mark, int columns,
                               int max_rows, int measure)
{
    const char *gutters[] = {UI_BAR " "};
    struct ui_wrap w = {0};
    w.budget = (size_t)(columns > 5 ? columns - 5 : 1);
    w.gutter = UI_BAR "   ";
    w.gutters = gutters;
    w.gutters_n = 1;
    w.mark = mark;
    w.role = UI_SIDE;
    w.max_rows = max_rows;
    w.erase = 1;
    w.paint_empty = 1;
    w.measure = measure;
    return ui_wrap_paint(question, &w);
}
