#include "taskrows.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "session.h"
#include "status.h"
#include "tasks.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define ROWS_MAX  5
#define DONE_SECS 2

static long painted_sig = -1;

static int shown(const struct task *a, time_t now)
{
    if (a->inferred)
        return 0;
    return !tasks_done(a) || (a->ended && now - a->ended <= DONE_SECS);
}

static int collect(const struct task **out, int max, int *total)
{
    const struct tasktab *t = session_tasks(workspace_current());
    time_t                now = time(NULL);
    int                   n = 0;

    *total = 0;
    for (int i = 0; t && i < tasks_count(t); i++) {
        const struct task *a = tasks_at(t, i);
        if (!shown(a, now))
            continue;
        if (n < max)
            out[n++] = a;
        (*total)++;
    }
    return n;
}

int taskrows_count(void)
{
    const struct task *v[ROWS_MAX];
    int                total;
    int                n = collect(v, ROWS_MAX, &total);
    return n + (total > n);
}

static long signature(int rows)
{
    if (!rows)
        return 0;
    long frame = (long)(now_seconds() * 1000.0 / SPIN_FRAME_MS);
    return frame * 8 + rows;
}

int taskrows_stale(void)
{
    int rows = taskrows_count();
    return (rows || painted_sig > 0) && signature(rows) != painted_sig;
}

void taskrows_paint(int cols)
{
    const struct task *v[ROWS_MAX];
    int                total;
    int                n = collect(v, ROWS_MAX, &total);
    long               frame = (long)(now_seconds() * 1000.0 / SPIN_FRAME_MS);
    time_t             now = time(NULL);
    char               kind[ROWS_MAX][96];
    size_t             kind_w = 0;

    painted_sig = signature(n + (total > n));
    for (int i = 0; i < n; i++) {
        tasks_kind(v[i], kind[i], sizeof kind[i]);
        if (ui_cells(kind[i]) > kind_w)
            kind_w = ui_cells(kind[i]);
    }

    for (int i = 0; i < n; i++) {
        const struct task *a = v[i];
        int                done = tasks_done(a);
        const char        *what = !done && a->latest[0] ? a->latest
                                  : a->cmd[0]           ? a->cmd
                                  : a->desc[0]          ? a->desc
                                                        : a->id;
        char               line[256];
        char               took[32];

        text_one_line(what, line, sizeof line);
        tasks_duration(took, sizeof took, (long)((done ? a->ended : now) - a->started));

        int left = 4 + (int)kind_w + 2;
        int room = cols - 1 - left - (int)strlen(took) - 2;

        ui_put("\n  ");
        if (done && !strcmp(a->status, "completed")) {
            ui_esc(ui_style(UI_OK));
            ui_put("\xe2\x9c\x93");
        } else if (done) {
            ui_esc(ui_style(UI_ERROR));
            ui_put("\xc3\x97");
        } else {
            ui_esc(ui_style(UI_DIM));
            ui_put(spin_glyph((int)frame));
        }
        ui_esc(ui_style(UI_RESET));
        ui_esc(ui_style(UI_DIM));
        ui_put(" ");
        ui_put(kind[i]);
        for (size_t w = ui_cells(kind[i]); w < kind_w + 2; w++)
            ui_put(" ");
        size_t fit = room > 0 ? ui_fit_bytes(line, (size_t)room) : 0;
        ui_putn(line, fit);
        int pad = room > 0 ? room - (int)ui_cells_n(line, fit) + 2 : 1;
        for (int p = 0; p < pad; p++)
            ui_put(" ");
        ui_put(took);
        ui_esc(ui_style(UI_RESET));
    }
    if (total > n) {
        char more[32];
        snprintf(more, sizeof more, "\n    +%d more", total - n);
        ui_esc(ui_style(UI_DIM));
        ui_put(more);
        ui_esc(ui_style(UI_RESET));
    }
}
