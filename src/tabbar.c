#include "tabbar.h"

#include <stdio.h>
#include <string.h>

#include "session.h"
#include "status.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define NAME_CELLS 14
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

/* the spinner over the input already counts out the current tab's turn */
static const char *mark(int at, enum ui_role *role)
{
    const struct session *s = workspace_at(at);
    const char           *status = workspace_status(s);

    if (!strcmp(status, "working") && at != workspace_index()) {
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

static void name_of(const struct session *s, char *out, size_t size)
{
    const char *title = session_title(s);
    if (!title || !*title)
        title = "untitled";

    size_t fit = ui_fit_visible(title, strlen(title), NAME_CELLS);
    snprintf(out, size, "%.*s%s", (int)fit, title, title[fit] ? "\xe2\x80\xa6" : "");
}

void tabbar_paint(int cols)
{
    int    n = workspace_count();
    size_t budget = cols > 1 ? (size_t)(cols - 1) : 1;
    int    left = n;
    size_t at = 1;

    spin_advance(&frame, &frame_at);
    painted = digest();
    spans = 0;
    ui_esc(UI_ERASE_EOL);

    for (int i = 0; i < n; i++) {
        struct session *s = workspace_at(i);
        enum ui_role    role = UI_DIM;
        const char     *glyph = mark(i, &role);
        char            name[256];

        name_of(s, name, sizeof name);

        int    here = i == workspace_index();
        size_t gap = i ? 2 : 0;
        size_t need = gap + (glyph ? 2 : 0) + (here ? 4 : 0) + ui_cells(name);
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
