#include "boardtile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boarddiff.h"
#include "boardflow.h"
#include "boardtriage.h"
#include "boardwork.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define WHERE_MAX 26

/* a fixed buffer can cut a multi-byte glyph in half */
static void trim_partial(char *s)
{
    size_t n = strlen(s), at = n;
    while (at && ((unsigned char)s[at - 1] & 0xc0) == 0x80)
        at--;
    if (!at)
        return;
    unsigned char lead = (unsigned char)s[at - 1];
    size_t        want = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3
                         : lead >= 0xc0                                    ? 2
                                                                           : 0;
    if (want && at - 1 + want > n)
        s[at - 1] = '\0';
}

static void column_mark(const struct board_card *c, const char **mark,
                        unsigned char *role)
{
    if (boardflow_waits_on_you(c)) {
        *mark = "\xe2\x9c\x93";
        *role = UI_OK;
        return;
    }

    switch (board_stands(c)) {
    case BOARD_OPEN:
        *mark = "●";
        *role = UI_DIM;
        break;
    case BOARD_CLOSED:
        *mark = "✓";
        *role = UI_DIM;
        break;
    default:
        *mark = NULL;
        *role = UI_DIM;
        break;
    }
}

static void short_repo(const char *cwd, char *out, size_t size)
{
    char full[4096];
    path_home_relative(cwd, full, sizeof full);
    if (ui_cells(full) <= WHERE_MAX) {
        snprintf(out, size, "%s", full);
        return;
    }
    const char *tail = full + strlen(full);
    const char *best = NULL;
    while (tail > full) {
        const char *slash = tail - 1;
        while (slash > full && *slash != '/')
            slash--;
        if (*slash != '/')
            break;
        if (ui_cells(slash) + 1 > WHERE_MAX)
            break;
        best = slash;
        tail = slash;
    }
    if (best)
        snprintf(out, size, "…%s", best);
    else
        snprintf(out, size, "…%s", strrchr(full, '/') ? strrchr(full, '/') : full);
}

const char *boardtile_step(const char *id)
{
    if (boardtriage_running(id))
        return "triaging";
    return boardwork_step_job(id);
}

static void status_of(const struct board_card *c, int wide, const char *step,
                      int tab, char *out, size_t size, time_t *stamp)
{
    char ts[32], when[64];
    text_ago(c->updated ? c->updated : c->created, c->col == BOARD_DONE, ts,
             sizeof ts);
    if (step && tab >= 0)
        snprintf(when, sizeof when, "%s · tab %d", step, tab + 1);
    else if (step)
        snprintf(when, sizeof when, "%s…", step);
    else {
        *stamp = c->updated ? c->updated : c->created;
        if (tab >= 0)
            snprintf(when, sizeof when, "tab %d · %s", tab + 1, ts);
        else
            snprintf(when, sizeof when, "%s", ts);
    }

    char where[256] = {0};
    if (wide)
        short_repo(c->cwd, where, sizeof where);

    char waiting[256] = {0};
    if (board_stands(c) == BOARD_WORKING && boardwork_tab(c->id) < 0)
        boardwork_blocked(c, waiting, sizeof waiting);

    if (waiting[0])
        snprintf(out, size, "waiting · %s", waiting);
    else if (boardflow_waits_on_you(c) && c->worktree[0]) {
        int files = 0, lines = 0;
        boarddiff_size_cached(c, &files, &lines);
        snprintf(out, size, "%d file%s, %d line%s", files, files == 1 ? "" : "s",
                 lines, lines == 1 ? "" : "s");
    } else if (c->cost_usd > 0 &&
               (boardflow_waits_on_you(c) ||
                board_stands(c) == BOARD_CLOSED)) {
        char head[320] = "";
        if (where[0])
            snprintf(head, sizeof head, "%s · ", where);
        snprintf(out, size, "%s$%.2f · %s", head, c->cost_usd, when);
    } else if (where[0])
        snprintf(out, size, "%s · %s", where, when);
    else
        snprintf(out, size, "%s", when);

    trim_partial(out);
}

int boardtile_of(const struct board_card *c, int wide, struct board_tile *out)
{
    if (!c || !out)
        return 0;

    memset(out, 0, sizeof *out);
    out->c = c;
    snprintf(out->title, sizeof out->title, "%s",
             c->title[0] ? c->title : "(untitled)");
    snprintf(out->kind, sizeof out->kind, "%s", c->kind);
    column_mark(c, &out->mark, &out->mark_role);

    out->tab = boardwork_tab(c->id);
    const char *step = boardtile_step(c->id);
    out->spin = (unsigned char)(step ||
                                (out->tab >= 0 &&
                                 board_stands(c) == BOARD_WORKING &&
                                 session_busy(workspace_at(out->tab))));

    status_of(c, wide, step, out->tab, out->status, sizeof out->status,
              &out->stamp);

    if (c->backend_pin[0] || c->tier_pin[0])
        snprintf(out->pins, sizeof out->pins, "%s%s%s",
                 c->backend_pin[0] ? c->backend_pin : "",
                 c->backend_pin[0] && c->tier_pin[0] ? " " : "",
                 c->tier_pin[0] ? c->tier_pin : "");

    if (out->tab >= 0)
        out->recent_n =
            session_recent(workspace_at(out->tab), out->recent, BOARD_RECENT);

    return 1;
}
