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
#define BUTTON_W   4

#define CHECK "\xe2\x9c\x93"
#define BAR "\xe2\x94\x82"
#define RULE "\xe2\x94\x80"

static const char *const METER[] = {
    "\xe2\xa0\x80", "\xe2\xa1\x80", "\xe2\xa3\x80", "\xe2\xa3\x84", "\xe2\xa3\xa4",
    "\xe2\xa3\xa6", "\xe2\xa3\xb6", "\xe2\xa3\xb7", "\xe2\xa3\xbf",
};

static int meter_step(const struct session *s)
{
    int pct = session_context_percent(s);
    if (pct < 0)
        return 0;
    return pct / 5 < 8 ? pct / 5 : 8;
}

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
        h = h * 16777619u + (unsigned)meter_step(s);
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
        *role = UI_OK;
        return CHECK;
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
    const char  *meter;
    enum ui_role role;
    int          cells;
};

static struct tab tabs[WORKSPACE_MAX];
static int        ntabs;
static int        box_col = -1;
static int        box_w;

static void rule(int cells)
{
    for (int i = 0; i < cells; i++)
        ui_put(RULE);
}

static void paint_row(void *ud, int line, int w)
{
    int at = *(int *)ud + line;

    ui_esc(ui_style(UI_DIM));
    if (at < ntabs) {
        const struct tab *t = &tabs[at];

        ui_put(BAR " ");
        if (t->glyph) {
            ui_esc(ui_style(t->role));
            ui_put(t->glyph);
            ui_put(" ");
        }
        ui_esc(ui_style(t->index == workspace_index() ? UI_ACCENT : UI_DIM));
        ui_put(t->name);
        ui_put(" ");
        ui_put(t->meter);
        ui_pad(w - 2 - t->cells);
    } else if (at == ntabs) {
        ui_put("\xe2\x95\xb0");
        rule(w - 1 - BUTTON_W);
        ui_put("\xe2\x94\xac");
        rule(BUTTON_W - 1);
    } else if (at == ntabs + 1) {
        ui_put(BAR " ");
        ui_esc(ui_style(UI_ACCENT));
        ui_put("+ ");
    } else {
        ui_put("\xe2\x95\xb0");
        rule(BUTTON_W - 1);
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
    if (count < 1 || cur < 0 || n < 4)
        return;
    if (count == 1 && (!settings_get_int(SETTING_NAME_BADGE, 1) ||
                       (!session_remote(workspace_at(cur)) && !session_name(workspace_at(cur))[0])))
        return;

    ntabs = count < n - 3 ? count : n - 3;
    int widest = 0;
    for (int i = 0; i < ntabs; i++) {
        struct tab           *t = &tabs[i];
        const struct session *s = workspace_at(i);

        t->index = i;
        name_of(s, t->name, sizeof t->name);
        t->role = UI_DIM;
        t->glyph = mark(s, &t->role);
        t->meter = METER[meter_step(s)];
        t->cells = (int)ui_cells(t->name) + 2 + (t->glyph ? 2 : 0);
        if (t->cells > widest)
            widest = t->cells;
    }
    int w = widest + 3;
    if (w < BUTTON_W + 2)
        w = BUTTON_W + 2;
    if (w > cols)
        return;

    int height = ntabs + 3;
    int            r;
    struct overlay o = {.col = cols - w, .w = w, .rows = 1, .paint_row = paint_row, .ud = &r};
    for (r = 0; r < height; r++) {
        o.w = r > ntabs ? BUTTON_W : w;
        o.col = cols - o.w;
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
    box_w = w;
}

int tabbar_hit(int row, int col)
{
    int at = row - 1;
    int x = col - 1 - box_col;

    if (box_col < 0 || x < 0)
        return TABBAR_NONE;
    if (at < ntabs)
        return tabs[at].index;
    if (at == ntabs + 1 && x >= box_w - BUTTON_W)
        return TABBAR_NEW;
    return TABBAR_NONE;
}
