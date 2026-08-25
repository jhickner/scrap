#include "app.h"
#include "boardcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardwork.h"
#include "form.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"


/* what a collapsed card shows of its spec and of its log */
#define CARD_SPEC_ROWS 6
#define CARD_LOG_ROWS  6

static const char *const PRIORITIES[] = {"0", "1", "2", "3"};

struct notes {
    const char   **v;
    char         **owned;
    const char   **labels;
    char         **owned_labels;
    enum ui_role  *roles;
    int            n, cap;
    int            log_from; /* the first note of the card's log, or -1 */
};

/* takes ownership of label and text */
static void note_at(struct notes *l, char *label, char *text, enum ui_role role)
{
    if (l->n == l->cap) {
        int          cap = l->cap ? l->cap * 2 : 32;
        const char **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (v)
            l->v = v;
        char **owned = realloc(l->owned, (size_t)cap * sizeof *owned);
        if (owned)
            l->owned = owned;
        const char **labels = realloc(l->labels, (size_t)cap * sizeof *labels);
        if (labels)
            l->labels = labels;
        char **owned_labels =
            realloc(l->owned_labels, (size_t)cap * sizeof *owned_labels);
        if (owned_labels)
            l->owned_labels = owned_labels;
        enum ui_role *roles = realloc(l->roles, (size_t)cap * sizeof *roles);
        if (roles)
            l->roles = roles;
        if (!v || !owned || !labels || !owned_labels || !roles) {
            free(label);
            free(text);
            return;
        }
        l->cap = cap;
    }
    l->owned[l->n] = text;
    l->v[l->n] = text ? text : "";
    l->owned_labels[l->n] = label;
    l->labels[l->n] = label;
    l->roles[l->n] = role;
    l->n++;
}

/* takes ownership of text */
static void note_role(struct notes *l, char *text, enum ui_role role)
{
    note_at(l, NULL, text, role);
}

static void note_line(struct notes *l, char *text)
{
    note_at(l, NULL, text, UI_DIM);
}

static void note_labelled(struct notes *l, const char *label, char *text)
{
    note_at(l, text_dsprintf("%s", label), text, UI_DIM);
}

static void notes_free(struct notes *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->owned[i]);
        free(l->owned_labels[i]);
    }
    free(l->labels);
    free(l->owned_labels);
    free(l->owned);
    free(l->roles);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* What the card has been through, what it is on, and what is waiting. */
static void build_stages(const struct board_card *c, struct notes *notes)
{
    char   line[512];
    size_t at = 0;
    line[0] = '\0';

    for (int i = 0; i < c->done_n && at < sizeof line; i++)
        at += (size_t)snprintf(line + at, sizeof line - at,
                               at ? "  **\xe2\x9c\x93 %s**" : "**\xe2\x9c\x93 %s**",
                               c->done[i]);

    for (int i = 0; i < c->queue_n && at < sizeof line; i++)
        at += (size_t)snprintf(line + at, sizeof line - at,
                               i ? (at ? "  \xe2\x97\x8b %s" : "\xe2\x97\x8b %s")
                                 : (at ? "  **\xe2\x96\xb8 %s**" : "**\xe2\x96\xb8 %s**"),
                               c->queue[i]);

    if (at)
        note_labelled(notes, "flow", text_dsprintf("%s", line));
}

/* The live session is the truth while a tab is up: a handover switches the
   backend under a running card before the card records it. */
static void build_backend(const struct board_card *c, struct notes *notes)
{
    int             tab = boardwork_tab(c->id);
    struct session *s = tab >= 0 ? workspace_at(tab) : NULL;
    const char     *live = s ? session_backend(s) : NULL;
    const char     *name = live && *live ? live : c->backend;

    if (!name || !*name)
        return;

    note_labelled(notes, "backend",
                  text_dsprintf("%s · %s", name, s ? "working" : "worked"));
}

static void build_spend(const struct board_card *c, struct notes *notes)
{
    char cost[32] = "";
    if (c->cost_usd > 0)
        snprintf(cost, sizeof cost, "$%.2f", c->cost_usd);

    if (!c->tokens_in && !c->tokens_out) {
        if (cost[0])
            note_labelled(notes, "spend", text_dsprintf("%s", cost));
        return;
    }

    char in[32], out[32];
    text_humanize(c->tokens_in, in, sizeof in);
    text_humanize(c->tokens_out, out, sizeof out);
    note_labelled(notes, "spend",
                  text_dsprintf("%s in · %s out%s%s", in, out,
                                cost[0] ? " · " : "", cost));
}

/* The stamp and who said it head the entry; the rest sits indented under it. */
static void note_entry(struct notes *notes, const char *head, const char *stamp,
                       const char *who, const char *text)
{
    const char *at = text;
    int         first = 1;

    do {
        const char *nl = strchr(at, '\n');
        size_t      len = nl ? (size_t)(nl - at) : strlen(at);

        if (first)
            note_at(notes, head ? text_dsprintf("%s", head) : NULL,
                    text_dsprintf("%s %-6s  %.*s", stamp, who, (int)len, at),
                    UI_DIM);
        else if (len)
            note_line(notes, text_dsprintf("  %.*s", (int)len, at));
        else
            note_line(notes, NULL);

        first = 0;
        at = nl ? nl + 1 : NULL;
    } while (at);
}

/* A worker's final message is a whole turn, not a lifecycle line: the stamp
   stands alone and the message sits under it at the card's own indent. */
static void note_message(struct notes *notes, const char *head, const char *stamp,
                         const char *who, const char *text)
{
    note_at(notes, head ? text_dsprintf("%s", head) : NULL,
            text_dsprintf("%s %s", stamp, who), UI_DIM);

    const char *at = text;
    do {
        const char *nl = strchr(at, '\n');
        size_t      len = nl ? (size_t)(nl - at) : strlen(at);

        if (len)
            note_role(notes, text_dsprintf("  %.*s", (int)len, at), UI_BODY);
        else
            note_line(notes, NULL);

        at = nl ? nl + 1 : NULL;
    } while (at);
}

static void build_notes(const struct board_card *c, struct notes *notes)
{
    notes->log_from = -1;

    build_stages(c, notes);
    build_backend(c, notes);
    build_spend(c, notes);

    const char *head = "log";
    for (int i = 0; i < c->log_n; i++) {
        if (i == 0)
            notes->log_from = notes->n;
        struct tm when;
        char      stamp[16] = "     ";
        if (c->log[i].ts) {
            localtime_r(&c->log[i].ts, &when);
            strftime(stamp, sizeof stamp, "%H:%M", &when);
        }
        char *text = ui_plain(c->log[i].text ? c->log[i].text : "", 0);
        if (text && strchr(text, '\n'))
            note_message(notes, head, stamp, c->log[i].who, text);
        else
            note_entry(notes, head, stamp, c->log[i].who, text ? text : "");
        head = NULL;
        free(text);
    }
}

enum boardcard_action boardcard_form(const struct board_card *c,
                                     struct boardcard_edit *out)
{
    const struct board_cfg *cfg = boardcfg();
    const char             *kinds[BOARD_KINDS_MAX + 1];
    int                     kinds_n = 0;
    kinds[kinds_n++] = "";
    for (int i = 0; i < cfg->kinds_n; i++)
        kinds[kinds_n++] = cfg->kinds[i].name;

    const char *backends[BOARD_BACKENDS_MAX + 1];
    int         backends_n =
        boardcfg_backend_choices(cfg, backends, COUNT(backends));
    const char *tiers[BOARD_TIERS + 1];
    int         tiers_n = boardcfg_tier_choices(tiers, COUNT(tiers));

    char spec[8192];
    char kind[16];
    char where[4096];
    char priority[8];
    char backend[32];
    char tier[8];

    snprintf(spec, sizeof spec, "%s", c->body ? c->body : "");
    snprintf(kind, sizeof kind, "%s", c->kind);
    path_home_relative(c->cwd, where, sizeof where);
    snprintf(priority, sizeof priority, "%d", c->priority);
    snprintf(backend, sizeof backend, "%s", c->backend_pin);
    snprintf(tier, sizeof tier, "%s", c->tier_pin);

    char unstart_at[2] = "";

    struct form_field fields[FORM_FIELDS];
    int               fields_n = 0;

    fields[fields_n++] = (struct form_field){"spec", FORM_TEXT, spec,
                                             sizeof spec, NULL, 0,
                                             CARD_SPEC_ROWS};
    fields[fields_n++] = (struct form_field){"kind", FORM_CHOICE, kind,
                                             sizeof kind, kinds, kinds_n, 0};
    fields[fields_n++] = (struct form_field){"repo", FORM_TEXT, where,
                                             sizeof where, NULL, 0, 0};
    fields[fields_n++] = (struct form_field){"priority", FORM_CHOICE, priority,
                                             sizeof priority, PRIORITIES,
                                             COUNT(PRIORITIES), 0};
    fields[fields_n++] = (struct form_field){"backend", FORM_CHOICE, backend,
                                             sizeof backend, backends,
                                             backends_n, 0};
    fields[fields_n++] = (struct form_field){"tier", FORM_CHOICE, tier,
                                             sizeof tier, tiers, tiers_n, 0};
    if (board_stands(c) == BOARD_WORKING)
        fields[fields_n++] = (struct form_field){
            "stop, and clear what is queued", FORM_BUTTON, unstart_at,
            sizeof unstart_at, NULL, 0, 0};
    static const char *const OPEN[] = {"show the whole card", "show less"};
    char                     open_at[2] = "";
    fields[fields_n++] = (struct form_field){NULL,    FORM_TOGGLE, open_at,
                                             sizeof open_at, OPEN, 2, 0};

    const char *offered[BOARD_ACTIONS_MAX + 1];
    int         offered_n = 0;
    offered[offered_n++] = "";
    offered_n += boardflow_offered(c, offered + 1, BOARD_ACTIONS_MAX);

    char run[32] = "";
    char run_at[2] = "";
    char close_at[2] = "";
    if (offered_n > 1) {
        fields[fields_n++] = (struct form_field){"run", FORM_CHOICE, run,
                                                 sizeof run, offered,
                                                 offered_n, 0};
        fields[fields_n++] = (struct form_field){"run it", FORM_BUTTON, run_at,
                                                 sizeof run_at, NULL, 0, 0};
    }
    if (board_stands(c) != BOARD_CLOSED)
        fields[fields_n++] = (struct form_field){"close the card", FORM_BUTTON,
                                                 close_at, sizeof close_at,
                                                 NULL, 0, 0};

    struct notes notes = {0};
    build_notes(c, &notes);

    char heading[600];
    if (c->kind[0] && c->title[0] && strcmp(c->title, c->body ? c->body : ""))
        snprintf(heading, sizeof heading, "%s · %s · %s", c->id, c->kind, c->title);
    else
        snprintf(heading, sizeof heading, "%s · %s", c->id,
                 c->kind[0] ? c->kind : "unsorted");

    struct form f = {
        .title = heading,
        .notes = notes.v,
        .note_labels = notes.labels,
        .notes_from = notes.log_from < 0 ? 0 : notes.log_from,
        .notes_max = notes.log_from < 0 ? 0 : CARD_LOG_ROWS,
        .note_roles = notes.roles,
        .notes_n = notes.n,
        .fields = fields,
        .fields_n = fields_n,
    };
    int kept = form_run(&f);

    notes_free(&notes);

    snprintf(out->spec, sizeof out->spec, "%s", spec);
    snprintf(out->kind, sizeof out->kind, "%s", kind);
    snprintf(out->where, sizeof out->where, "%s", where);
    snprintf(out->run, sizeof out->run, "%s", run);
    snprintf(out->priority, sizeof out->priority, "%s", priority);
    snprintf(out->backend, sizeof out->backend, "%s", backend);
    snprintf(out->tier, sizeof out->tier, "%s", tier);

    if (unstart_at[0])
        return BOARDCARD_UNSTART;
    if (run_at[0] && run[0])
        return BOARDCARD_RUN;
    if (close_at[0])
        return BOARDCARD_CLOSE;
    return kept ? BOARDCARD_SAVE : BOARDCARD_NONE;
}
