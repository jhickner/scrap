#include "boardcfgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ask.h"
#include "boardcfg.h"
#include "form.h"
#include "pick.h"
#include "vendor/agents/backend.h"

#define CFG_HINT "enter edit  \xc2\xb7  esc done"

enum row_kind {
    ROW_HEAD,
    ROW_COUNT,
    ROW_TOGGLE,
    ROW_SERVING,
    ROW_BACKEND,
    ROW_VIEW,
};

struct row {
    enum row_kind kind;
    const char   *label;
    const char   *about;

    int *count;
    int  low, high;
    const char *units;

    int backend_at;
};

#define ROWS_MAX 64

static void head(struct row *rows, int *n, const char *label)
{
    rows[*n].kind = ROW_HEAD;
    rows[*n].label = label;
    (*n)++;
}

static void count_row(struct row *rows, int *n, const char *label, int *value,
                      int low, int high, const char *units)
{
    rows[*n].kind = ROW_COUNT;
    rows[*n].label = label;
    rows[*n].count = value;
    rows[*n].low = low;
    rows[*n].high = high;
    rows[*n].units = units;
    (*n)++;
}

static void toggle_row(struct row *rows, int *n, const char *label, int *value)
{
    rows[*n].kind = ROW_TOGGLE;
    rows[*n].label = label;
    rows[*n].count = value;
    (*n)++;
}

static void build(struct row *rows, int *n, struct board_cfg *c)
{
    *n = 0;

    head(rows, n, "workers");
    count_row(rows, n, "concurrency", &c->workers, 1, 11, NULL);
    toggle_row(rows, n, "auto pull", &c->auto_pull);
    toggle_row(rows, n, "auto pick", &c->auto_pick);

    head(rows, n, "archive");
    count_row(rows, n, "after", &c->archive_after, 0, 3650, "days");

    head(rows, n, "board");
    rows[*n].kind = ROW_VIEW;
    rows[*n].label = "view";
    (*n)++;

    head(rows, n, "columns");
    count_row(rows, n, "done shown", &c->done_shown, 0, 500, "cards");
    count_row(rows, n, "open shown", &c->open_shown, 0, 500, "cards");

    head(rows, n, "backends");
    rows[*n].kind = ROW_SERVING;
    rows[*n].label = "serving";
    (*n)++;
    for (int i = 0; i < c->backends_n && *n < ROWS_MAX - 1; i++) {
        rows[*n].kind = ROW_BACKEND;
        rows[*n].label = c->backends[i].name;
        rows[*n].backend_at = i;
        (*n)++;
    }
}

static void value_of(const struct row *r, const struct board_cfg *c,
                     char *out, size_t size)
{
    switch (r->kind) {
    case ROW_COUNT:
        if (!*r->count && r->low == 0)
            snprintf(out, size, "never");
        else if (r->units)
            snprintf(out, size, "%d%s%s", *r->count,
                     r->units[0] == '%' ? "" : " ", r->units);
        else
            snprintf(out, size, "%d", *r->count);
        break;
    case ROW_TOGGLE:
        snprintf(out, size, "%s", *r->count ? "yes" : "no");
        break;
    case ROW_SERVING:
        snprintf(out, size, "%s", c->serving);
        break;
    case ROW_BACKEND: {
        const struct board_backend *b = &c->backends[r->backend_at];
        size_t                      at = 0;
        out[0] = '\0';
        for (int t = 0; t < BOARD_TIERS && at < size; t++)
            at += (size_t)snprintf(out + at, size - at, "%s%s", t ? " · " : "",
                                   b->level[t].model[0] ? b->level[t].model : "default");
        break;
    }
    case ROW_VIEW:
        snprintf(out, size, "%s", c->view[0] ? c->view : "list");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void edit_count(struct row *r)
{
    char now[64], title[160];
    snprintf(now, sizeof now, "%d", *r->count);
    snprintf(title, sizeof title, "%s%s%s", r->label,
             r->units ? " — " : "", r->units ? r->units : "");

    char *said = ask_run(title, now);
    if (!said)
        return;
    char *end = NULL;
    long  want = strtol(said, &end, 10);
    free(said);
    if (end == NULL || want < r->low || want > r->high)
        return;
    *r->count = (int)want;
}

static const char *const EFFORTS[] = {
    "", "low", "medium", "high", "xhigh", "max",
};

static void edit_serving(struct board_cfg *c)
{
    if (!c->backends_n)
        return;

    struct pick_item items[BOARD_BACKENDS_MAX];
    int              at = 0;
    for (int i = 0; i < c->backends_n; i++) {
        items[i] = (struct pick_item){c->backends[i].name, NULL};
        if (!strcmp(c->backends[i].name, c->serving))
            at = i;
    }

    int chosen = pick_run("serving the board", items, c->backends_n, at);
    if (chosen >= 0)
        snprintf(c->serving, sizeof c->serving, "%s", c->backends[chosen].name);
}

static void edit_backend(struct board_cfg *c, int at)
{
    struct board_backend *b = &c->backends[at];

    char              model[BOARD_TIERS][128], effort[BOARD_TIERS][32];
    char              labels[BOARD_TIERS * 2][32];
    struct form_field fields[BOARD_TIERS * 2];
    int               n = 0;

    for (int t = 0; t < BOARD_TIERS; t++) {
        const char *name = boardcfg_tier_name((enum board_tier)t);
        snprintf(model[t], sizeof model[t], "%s", b->level[t].model);
        snprintf(effort[t], sizeof effort[t], "%s", b->level[t].effort);

        snprintf(labels[n], sizeof labels[n], "%s model", name);
        fields[n] = (struct form_field){labels[n], FORM_TEXT, model[t],
                                        sizeof model[t], NULL, 0, 0};
        n++;
        snprintf(labels[n], sizeof labels[n], "%s effort", name);
        fields[n] = (struct form_field){labels[n], FORM_CHOICE, effort[t],
                                        sizeof effort[t], EFFORTS, COUNT(EFFORTS), 0};
        n++;
    }

    static const char *const NOTES[] = {
        "what low, med and high mean on this backend.",
        "empty = the backend's own default",
    };

    struct form f = {.title = b->name, .notes = NOTES, .notes_n = 2,
                     .fields = fields, .fields_n = n};
    if (!form_run(&f))
        return;

    for (int t = 0; t < BOARD_TIERS; t++) {
        snprintf(b->level[t].model, sizeof b->level[t].model, "%s", model[t]);
        snprintf(b->level[t].effort, sizeof b->level[t].effort, "%s", effort[t]);
    }
}


void boardcfgui_run(void)
{
    struct board_cfg *c = boardcfg_copy();
    if (!c)
        return;

    struct row rows[ROWS_MAX];
    int        n = 0;
    int        at = 0;
    int        touched = 0;

    for (;;) {
        build(rows, &n, c);

        struct pick_item items[ROWS_MAX];
        unsigned char    heading[ROWS_MAX];
        char             values[ROWS_MAX][256];

        for (int i = 0; i < n; i++) {
            items[i].label = rows[i].label;
            value_of(&rows[i], c, values[i], sizeof values[i]);
            items[i].detail = values[i][0] ? values[i] : NULL;
            heading[i] = rows[i].kind == ROW_HEAD ? PICK_HEADING : 0;
        }

        struct pick_live live = {.heading = heading, .hint = CFG_HINT, .align = 1};
        at = pick_run_live("board config", items, n, at, &live, PICK_SEARCH_SLASH,
                           NULL, NULL);
        if (at < 0)
            break;

        switch (rows[at].kind) {
        case ROW_COUNT:   edit_count(&rows[at]); touched = 1; break;
        case ROW_TOGGLE:  *rows[at].count = !*rows[at].count; touched = 1; break;
        case ROW_SERVING: edit_serving(c); touched = 1; break;
        case ROW_BACKEND: edit_backend(c, rows[at].backend_at); touched = 1; break;
        case ROW_VIEW:
            snprintf(c->view, sizeof c->view, "%s",
                     strcmp(c->view, "grid") ? "grid" : "list");
            touched = 1;
            break;
        default:          break;
        }
    }

    if (touched)
        boardcfg_set(c);
    boardcfg_free(c);
}
