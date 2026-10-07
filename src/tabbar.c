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
#define STATUS_CELLS 32

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
        h = h * 16777619u + (unsigned)session_in_turn(s);
        h = h * 16777619u + (unsigned)meter_step(s);
        for (const char *p = name ? name : ""; *p; p++)
            h = h * 16777619u + (unsigned char)*p;
        for (const char *p = session_title(s) ? session_title(s) : ""; *p; p++)
            h = h * 16777619u + (unsigned char)*p;
    }
    return h;
}

static unsigned painted;

static int spin_due(void)
{
    for (int i = 0; i < workspace_count(); i++)
        if (session_in_turn(workspace_at(i)))
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

    if (session_in_turn(s)) {
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

static void fit_cells(const char *text, size_t cells, char *out, size_t size)
{
    size_t len = strlen(text);
    size_t fit = ui_fit_visible(text, len, cells);
    if (fit == len) {
        snprintf(out, size, "%s", text);
        return;
    }
    fit = ui_fit_visible(text, len, cells - 1);
    snprintf(out, size, "%.*s\xe2\x80\xa6", (int)fit, text);
}

static void name_of(const struct session *s, char *out, size_t size, char *status,
                    size_t status_size)
{
    char        at[256];
    const char *title = session_title(s) ? session_title(s) : "";

    status[0] = '\0';
    if (session_remote(s) || session_name(s)[0]) {
        session_address(s, at, sizeof at);
        snprintf(out, size, "%s", at);
        if (strcmp(title, at) && strcmp(title, at + 1))
            fit_cells(title, STATUS_CELLS, status, status_size);
    } else {
        fit_cells(title, NAME_CELLS, out, size);
    }
}

struct tab {
    int          index;
    char         name[256];
    char         status[256];
    const char  *glyph;
    const char  *meter;
    int          step;
    enum ui_role role;
};

static struct tab tabs[WORKSPACE_MAX];
static int        ntabs;
static int        box_col = -1;
static int        col_glyph, col_name, col_status, col_meter;

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
        } else {
            ui_pad(col_glyph);
        }
        ui_esc(ui_style(t->index == workspace_index() ? UI_ACCENT : UI_DIM));
        ui_put(t->name);
        ui_pad(col_name - (int)ui_cells(t->name));
        if (col_status) {
            ui_esc(ui_style(UI_DIM));
            ui_put(" ");
            ui_put(t->status);
            ui_pad(col_status - (int)ui_cells(t->status));
        }
        if (t->meter) {
            ui_put(" ");
            if (t->step > 4)
                ui_esc(ui_style(UI_ERROR));
            ui_put(t->meter);
        } else {
            ui_pad(col_meter);
        }
        ui_pad(w - 2 - col_glyph - col_name - (col_status ? 1 + col_status : 0) - col_meter);
    } else {
        ui_put("\xe2\x95\xb0");
        rule(w - 1);
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

    ntabs = count < n - 1 ? count : n - 1;
    col_glyph = col_name = col_status = col_meter = 0;
    for (int i = 0; i < ntabs; i++) {
        struct tab           *t = &tabs[i];
        const struct session *s = workspace_at(i);

        t->index = i;
        name_of(s, t->name, sizeof t->name, t->status, sizeof t->status);
        t->role = UI_DIM;
        t->glyph = mark(s, &t->role);
        t->step = meter_step(s);
        t->meter = t->step ? METER[t->step] : NULL;
        if (t->glyph)
            col_glyph = 2;
        if ((int)ui_cells(t->name) > col_name)
            col_name = (int)ui_cells(t->name);
        if ((int)ui_cells(t->status) > col_status)
            col_status = (int)ui_cells(t->status);
        if (t->meter)
            col_meter = 1 + (int)ui_cells(t->meter);
    }
    int widest = col_glyph + col_name + (col_status ? 1 + col_status : 0) + col_meter;
    int w = widest + 3;
    if (w > cols)
        return;

    int height = ntabs + 1;
    int            r;
    struct overlay o = {.col = cols - w, .w = w, .rows = 1, .paint_row = paint_row, .ud = &r};
    for (r = 0; r < height; r++) {
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
    int x = col - 1 - box_col;

    if (box_col < 0 || x < 0)
        return TABBAR_NONE;
    if (at < ntabs)
        return tabs[at].index;
    return TABBAR_NONE;
}
