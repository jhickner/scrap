#include "tabbar.h"

#include <stdio.h>
#include <string.h>

#include "session.h"
#include "status.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define NAME_CELLS 14
#define NAME_CELLS_MIN 4
#define MIN_COLS   24

#define DOT "\xe2\x97\x8f"

static int    frame;
static double frame_at;

int tabbar_rows(int cols)
{
    return workspace_count() > 1 && cols >= MIN_COLS;
}

static unsigned digest(void)
{
    unsigned h = 2166136261u;
    int      n = workspace_count();

    h = h * 16777619u + (unsigned)n;
    h = h * 16777619u + (unsigned)workspace_index();
    for (int i = 0; i < n; i++) {
        const struct session *s = workspace_at(i);
        const char           *title = session_title(s);

        for (const char *p = workspace_status(s); *p; p++)
            h = h * 16777619u + (unsigned char)*p;
        h = h * 16777619u + (unsigned)(session_unseen(s) ? 1 : 0);
        for (const char *p = title ? title : ""; *p; p++)
            h = h * 16777619u + (unsigned char)*p;
    }
    return h;
}

static unsigned painted;

/* the columns each tab's entry was painted across, so a click can name one */
static struct { int start, end; } span[WORKSPACE_MAX];
static int spans;

static int spin_due(void)
{
    for (int i = 0; i < workspace_count(); i++)
        if (i != workspace_index() &&
            !strcmp(workspace_status(workspace_at(i)), "working"))
            return (now_seconds() - frame_at) * 1000.0 >= SPIN_FRAME_MS;
    return 0;
}

int tabbar_stale(void)
{
    if (workspace_count() < 2)
        return 0;
    return digest() != painted || spin_due();
}

/* a turn on the current tab is counted out above the prompt; everything else
   that is working, including this tab's leftover background work, marks here */
static const char *mark(int at, enum ui_role *role)
{
    const struct session *s = workspace_at(at);
    const char           *status = workspace_status(s);

    if (!strcmp(status, "working") &&
        (at != workspace_index() || !session_turn_running(s))) {
        *role = UI_SPIN;
        return spin_glyph(frame);
    }
    if (!strcmp(status, "errored")) {
        *role = UI_ERROR;
        return "e";
    }
    if (session_unseen(s)) {
        *role = UI_ACCENT;
        return DOT;
    }
    return NULL;
}

static void name_of(const struct session *s, size_t cells, char *out, size_t size)
{
    const char *title = session_title(s);
    if (!title || !*title)
        title = "untitled";

    size_t len = strlen(title);
    size_t fit = ui_fit_visible(title, len, cells);
    if (fit == len) {
        snprintf(out, size, "%s", title);
        return;
    }
    fit = ui_fit_visible(title, len, cells - 1);
    snprintf(out, size, "%.*s\xe2\x80\xa6", (int)fit, title);
}

/* fixed cost of a tab's entry: separator, mark, and the current tab's brackets */
static size_t frame_cells(int at)
{
    enum ui_role role;
    return (at ? 2 : 0) + (mark(at, &role) ? 2 : 0) +
           (at == workspace_index() ? 4 : 0);
}

/* the widest name cap that still fits every tab, or 0 if none does */
static size_t name_cells(int n, size_t budget)
{
    for (size_t cap = NAME_CELLS; cap >= NAME_CELLS_MIN; cap--) {
        size_t need = 0;
        for (int i = 0; i < n; i++) {
            char name[256];
            name_of(workspace_at(i), cap, name, sizeof name);
            need += frame_cells(i) + ui_cells(name);
        }
        if (need <= budget)
            return cap;
    }
    return 0;
}

void tabbar_paint(int cols)
{
    int    n = workspace_count();
    size_t budget = cols > 1 ? (size_t)(cols - 1) : 1;
    int    left = n;
    size_t at = 1;

    spin_advance(&frame, &frame_at);
    painted = digest();
    size_t cells = name_cells(n, budget);
    if (!cells)
        cells = NAME_CELLS_MIN;
    spans = 0;
    ui_esc(UI_ERASE_EOL);

    for (int i = 0; i < n; i++) {
        struct session *s = workspace_at(i);
        enum ui_role    role = UI_DIM;
        const char     *glyph = mark(i, &role);
        char            name[256];

        name_of(s, cells, name, sizeof name);

        int    here = i == workspace_index();
        size_t gap = i ? 2 : 0;
        size_t need = frame_cells(i) + ui_cells(name);
        if (need > budget)
            break;

        if (i) {
            ui_esc(ui_style(UI_DIM));
            ui_put("  ");
        }
        if (glyph) {
            ui_esc(ui_style(role));
            ui_put(glyph);
            ui_put(" ");
        }
        ui_esc(ui_style(here ? UI_TEXT : UI_DIM));
        if (here)
            ui_put("[ ");
        ui_put(name);
        if (here)
            ui_put(" ]");

        span[spans].start = (int)(at + gap);
        span[spans].end = (int)(at + need - 1);
        spans++;

        at += need;
        budget -= need;
        left--;
    }

    if (left > 0) {
        char rest[16];
        snprintf(rest, sizeof rest, "  +%d", left);
        if (ui_cells(rest) <= budget) {
            ui_esc(ui_style(UI_DIM));
            ui_put(rest);
        }
    }
    ui_esc(ui_style(UI_RESET));
}

int tabbar_hit(int col)
{
    for (int i = 0; i < spans; i++)
        if (col >= span[i].start && col <= span[i].end)
            return i;
    return -1;
}
