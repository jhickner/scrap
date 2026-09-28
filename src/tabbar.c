#include "tabbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define NAME_CELLS 14
#define INSET      2

#define DOT "\xe2\x97\x8f"
#define BAR "\xe2\x94\x82"
#define RULE "\xe2\x94\x80"

static int    frame;
static double frame_at;

static unsigned digest(void)
{
    unsigned h = 2166136261u;
    int      n = workspace_count();

    h = h * 16777619u + (unsigned)n;
    h = h * 16777619u + (unsigned)workspace_index();
    for (int i = 0; i < n; i++) {
        const struct session *s = workspace_at(i);
        const char           *name = session_name(s);

        for (const char *p = workspace_status(s); *p; p++)
            h = h * 16777619u + (unsigned char)*p;
        h = h * 16777619u + (unsigned)(session_unseen(s) ? 1 : 0);
        for (const char *p = name ? name : ""; *p; p++)
            h = h * 16777619u + (unsigned char)*p;
    }
    return h;
}

static unsigned painted;

static int spin_due(void)
{
    for (int i = 0; i < workspace_count(); i++)
        if (!strcmp(workspace_status(workspace_at(i)), "working"))
            return (now_seconds() - frame_at) * 1000.0 >= SPIN_FRAME_MS;
    return 0;
}

int tabbar_stale(void)
{
    return digest() != painted || spin_due();
}

static const char *mark(const struct session *s, enum ui_role *role)
{
    const char *status = workspace_status(s);

    if (!strcmp(status, "working")) {
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
    char        at[256];
    const char *name = at;

    if (session_remote(s) || session_name(s)[0])
        session_address(s, at, sizeof at);
    else
        name = session_title(s) ? session_title(s) : "";

    size_t len = strlen(name);
    size_t fit = ui_fit_visible(name, len, NAME_CELLS);
    if (fit == len) {
        snprintf(out, size, "%s", name);
        return;
    }
    fit = ui_fit_visible(name, len, NAME_CELLS - 1);
    snprintf(out, size, "%.*s\xe2\x80\xa6", (int)fit, name);
}

struct tab {
    int          index;
    char         name[256];
    const char  *glyph;
    enum ui_role role;
    int          cells;
};

static struct tab tabs[WORKSPACE_MAX];
static int        ntabs;
static int        box_col = -1;

static void rule(int cells)
{
    for (int i = 0; i < cells; i++)
        ui_put(RULE);
}

static void paint_row(void *ud, int at, int w)
{
    (void)ud;
    int last = ntabs > 1 ? ntabs + 1 : 1;
    int inset = at >= 2 ? INSET : 0;

    ui_esc(ui_style(UI_DIM));
    if (at == 1) {
        ui_put("\xe2\x95\xb0");
        if (ntabs > 1) {
            ui_put(RULE "\xe2\x94\xac");
            rule(w - 3);
        } else {
            rule(w - 1);
        }
    } else if (at == last) {
        ui_pad(INSET);
        ui_put("\xe2\x95\xb0");
        rule(w - INSET - 1);
    } else {
        const struct tab *t = &tabs[at ? at - 1 : 0];

        ui_pad(inset);
        ui_put(BAR " ");
        if (t->glyph) {
            ui_esc(ui_style(t->role));
            ui_put(t->glyph);
            ui_put(" ");
        }
        ui_esc(ui_style(at ? UI_DIM : UI_ACCENT));
        ui_put(t->name);
        ui_pad(w - inset - 2 - t->cells);
    }
    ui_esc(ui_style(UI_RESET));
}

void tabbar_cover(char **rows, int n, int cols)
{
    int count = workspace_count();
    int cur = workspace_index();

    box_col = -1;
    spin_advance(&frame, &frame_at);
    painted = digest();
    if (count < 1 || cur < 0 || n < 2)
        return;
    if (count == 1 && (!settings_get_int(SETTING_NAME_BADGE, 1) ||
                       (!session_remote(workspace_at(cur)) && !session_name(workspace_at(cur))[0])))
        return;

    ntabs = 0;
    tabs[ntabs++].index = cur;
    for (int i = 0; i < count && ntabs < n - 1; i++)
        if (i != cur)
            tabs[ntabs++].index = i;

    int widest = 0;
    for (int i = 0; i < ntabs; i++) {
        struct tab           *t = &tabs[i];
        const struct session *s = workspace_at(t->index);

        name_of(s, t->name, sizeof t->name);
        t->role = UI_DIM;
        t->glyph = mark(s, &t->role);
        t->cells = (int)ui_cells(t->name) + (t->glyph ? 2 : 0);
        if (t->cells > widest)
            widest = t->cells;
    }
    int w = widest + 3 + (ntabs > 1 ? INSET : 0);
    if (w > cols)
        return;

    int height = ntabs > 1 ? ntabs + 2 : 2;
    struct overlay o = {.col = cols - w, .w = w, .rows = height, .paint_row = paint_row};
    for (int r = 0; r < height; r++) {
        o.row = -r;
        ui_sink_begin();
        overlay_put(rows[r] ? rows[r] : "", &o);
        char *out = ui_sink_end();
        if (!out)
            continue;
        size_t len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = '\0';
        free(rows[r]);
        rows[r] = out;
    }
    box_col = cols - w;
}

int tabbar_hit(int row, int col)
{
    int at = row - 1;
    int inset = at >= 2 ? INSET : 0;

    if (box_col < 0 || col - 1 < box_col + inset)
        return -1;
    if (at == 0)
        return tabs[0].index;
    if (ntabs > 1 && at >= 2 && at <= ntabs)
        return tabs[at - 1].index;
    return -1;
}
