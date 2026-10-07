#include "taskrows.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "session.h"
#include "highlight.h"
#include "status.h"
#include "tasks.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define ROWS_MAX  5
#define DONE_SECS 2

#define CMD_INDENT 6

static long painted_sig = -1;
static char open_id[40];
static struct {
    int  line;
    char id[40];
} hits[64];
static int nhits;
static int focus_at = -1;

static int is_open(const struct task *a)
{
    return a->cmd[0] && !strcmp(a->id, open_id);
}

static int cmd_rows(const struct task *a, int cols)
{
    if (!is_open(a))
        return 0;
    struct ui_wrap w = {0};
    w.budget = (size_t)(cols - 1 - CMD_INDENT > 1 ? cols - 1 - CMD_INDENT : 1);
    w.measure = 1;
    return ui_wrap_paint(a->cmd, &w);
}

static void hit(const struct task *a)
{
    if (!a->cmd[0] || nhits >= (int)(sizeof hits / sizeof hits[0]))
        return;
    hits[nhits].line = ui_sink_rows();
    snprintf(hits[nhits].id, sizeof hits[nhits].id, "%s", a->id);
    nhits++;
}

static void paint_cmd(const struct task *a, int cols)
{
    size_t        len = strlen(a->cmd);
    size_t        budget = (size_t)(cols - 1 - CMD_INDENT > 1 ? cols - 1 - CMD_INDENT : 1);
    unsigned char roles[sizeof a->cmd];
    const char   *p = a->cmd;
    size_t        n = len;

    highlight_shell(a->cmd, len, roles);
    while (n) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, n, budget, &skip, NULL);
        ui_put("\n");
        hit(a);
        for (int i = 0; i < CMD_INDENT; i++)
            ui_put(" ");
        ui_put_spans(p, row, roles + (p - a->cmd), UI_RESET);
        ui_esc(ui_style(UI_RESET));
        p += row + skip;
        n -= row + skip < n ? row + skip : n;
    }
}

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

int taskrows_count(int cols)
{
    const struct task *v[ROWS_MAX];
    int                total;
    int                n = collect(v, ROWS_MAX, &total);
    int                rows = n + (total > n);
    for (int i = 0; i < n; i++)
        rows += cmd_rows(v[i], cols);
    return rows;
}

static void toggle(const char *id)
{
    if (!strcmp(open_id, id))
        open_id[0] = '\0';
    else
        snprintf(open_id, sizeof open_id, "%s", id);
}

int taskrows_click(int row)
{
    int top = viewport_chrome_top();
    if (top < 0)
        return 0;
    for (int i = 0; i < nhits; i++) {
        if (hits[i].line != row - 1 - top)
            continue;
        toggle(hits[i].id);
        return 1;
    }
    return 0;
}

int taskrows_items(void)
{
    const struct task *v[ROWS_MAX];
    int                total;
    return collect(v, ROWS_MAX, &total);
}

void taskrows_focus(int i) { focus_at = i; }

void taskrows_toggle(int i)
{
    const struct task *v[ROWS_MAX];
    int                total;
    if (i >= 0 && i < collect(v, ROWS_MAX, &total) && v[i]->cmd[0])
        toggle(v[i]->id);
}

int taskrows_stop(int i)
{
    const struct task *v[ROWS_MAX];
    int                total;
    if (i < 0 || i >= collect(v, ROWS_MAX, &total) || tasks_done(v[i]))
        return 0;
    return session_stop_task(workspace_current(), v[i]->id);
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
    int rows = taskrows_count(ui_columns());
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

    painted_sig = signature(taskrows_count(cols));
    nhits = 0;
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

        ui_put("\n");
        hit(a);
        if (i == focus_at) {
            ui_esc(ui_style(UI_ACCENT));
            ui_put("\xe2\x96\xb8 ");
        } else {
            ui_put("  ");
        }
        if (done && !strcmp(a->status, "completed")) {
            ui_esc(ui_style(UI_OK));
            ui_put("\xe2\x9c\x93");
        } else if (done) {
            ui_esc(ui_style(UI_ERROR));
            ui_put("\xc3\x97");
        } else {
            ui_esc(ui_style(UI_SPIN));
            ui_put(spin_glyph((int)frame));
        }
        ui_esc(ui_style(UI_RESET));
        ui_esc(ui_style(UI_DIM));
        ui_put(" ");
        ui_put(kind[i]);
        for (size_t w = ui_cells(kind[i]); w < kind_w + 2; w++)
            ui_put(" ");
        size_t fit = room > 0 ? ui_fit_bytes(line, (size_t)room) : 0;
        if (what == a->cmd) {
            unsigned char roles[sizeof line];
            highlight_shell(line, strlen(line), roles);
            ui_put_spans(line, fit, roles, UI_RESET);
            ui_esc(ui_style(UI_RESET));
        } else {
            ui_putn(line, fit);
        }
        int pad = room > 0 ? room - (int)ui_cells_n(line, fit) + 2 : 1;
        for (int p = 0; p < pad; p++)
            ui_put(" ");
        ui_esc(ui_style(UI_RESET));
        ui_esc(ui_style(UI_SPIN));
        ui_put(took);
        ui_esc(ui_style(UI_RESET));
        if (is_open(a))
            paint_cmd(a, cols);
    }
    if (total > n) {
        char more[32];
        snprintf(more, sizeof more, "\n    +%d more", total - n);
        ui_esc(ui_style(UI_DIM));
        ui_put(more);
        ui_esc(ui_style(UI_RESET));
    }
}
