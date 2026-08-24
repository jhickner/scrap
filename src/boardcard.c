#include "app.h"
#include "boardcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardplan.h"
#include "boardsweep.h"
#include "boardtriage.h"
#include "boardwork.h"
#include "form.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define STEPS_SHOWN (BOARD_KINDS_MAX * BOARD_KIND_STEPS)

static const char *const PRIORITIES[] = {"0", "1", "2", "3"};

struct notes {
    const char   **v;
    char         **owned;
    enum ui_role  *roles;
    int            n, cap;
};

/* takes ownership of text */
static void note_role(struct notes *l, char *text, enum ui_role role)
{
    if (l->n == l->cap) {
        int          cap = l->cap ? l->cap * 2 : 32;
        const char **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (v)
            l->v = v;
        char **owned = realloc(l->owned, (size_t)cap * sizeof *owned);
        if (owned)
            l->owned = owned;
        enum ui_role *roles = realloc(l->roles, (size_t)cap * sizeof *roles);
        if (roles)
            l->roles = roles;
        if (!v || !owned || !roles) {
            free(text);
            return;
        }
        l->cap = cap;
    }
    l->owned[l->n] = text;
    l->v[l->n] = text ? text : "";
    l->roles[l->n] = role;
    l->n++;
}

static void note_line(struct notes *l, char *text)
{
    note_role(l, text, UI_DIM);
}

static void notes_free(struct notes *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->owned[i]);
    free(l->owned);
    free(l->roles);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int stage_ran(const struct board_card *c, const char *who)
{
    for (int i = 0; i < c->log_n; i++)
        if (!strcmp(c->log[i].who, who))
            return 1;
    return 0;
}

struct stage {
    const char *name;
    const char *step;
    const char *who;
};

static int stages_of(const struct board_card *c, struct stage *out, int max)
{
    int n = 0;
    out[n++] = (struct stage){"triage", NULL, "triage"};

    for (int at = 0; n < max; at++) {
        const char *step = boardcfg_kind_step(c->kind, at);
        if (!step)
            break;
        const struct board_role *p = boardcfg_for_step(step);
        if (!p)
            continue;
        out[n++] = (struct stage){step, step,
                                  p->runs == BOARD_RUNS_PERSON  ? "you"
                                  : p->runs == BOARD_RUNS_COMMAND ? NULL
                                                                  : p->job};
    }
    return n;
}

static int stage_rank(const struct board_card *c, const struct stage *st)
{
    if (!st->step)
        return c->col > BOARD_NEW && c->col != BOARD_UNCLEAR ? 1 : 0;
    if (board_at(c, st->step))
        return 0;
    if (c->col == BOARD_DONE)
        return 1;
    if (c->col < BOARD_STEP)
        return -1;

    int here = boardcfg_kind_step_at(c->kind, c->step);
    int there = boardcfg_kind_step_at(c->kind, st->step);
    if (here < 0)
        return -1;
    return here > there ? 1 : -1;
}

static void build_stages(const struct board_card *c, struct notes *notes)
{
    struct stage STAGE[BOARD_KIND_STEPS + 1];
    int          stages = stages_of(c, STAGE, BOARD_KIND_STEPS + 1);

    char   line[512];
    size_t at = (size_t)snprintf(line, sizeof line, "  %-8s", "flow");
    int    any = 0;

    for (int i = 0; i < stages; i++) {
        if (at >= sizeof line)
            break;

        const char *mark;
        int         strong;
        int         rank = stage_rank(c, &STAGE[i]);
        if (rank == 0) {
            mark = "\xe2\x96\xb8";
            strong = 1;
        } else if (rank < 0) {
            mark = "\xe2\x97\x8b";
            strong = 0;
        } else if (STAGE[i].who && !stage_ran(c, STAGE[i].who)) {
            mark = "\xe2\x80\x93";
            strong = 0;
        } else {
            mark = "\xe2\x9c\x93";
            strong = 1;
        }

        int put = snprintf(line + at, sizeof line - at,
                           strong ? "  **%s %s**" : "  %s %s", mark,
                           STAGE[i].name);
        if (put < 0)
            break;
        at += (size_t)put;
        any = 1;
    }

    if (any)
        note_line(notes, text_dsprintf("%s", line));
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

    note_line(notes, text_dsprintf("  %-8s %s · %s", "backend", name,
                                   s ? "working" : "worked"));
}

static void build_spend(const struct board_card *c, struct notes *notes)
{
    char cost[32] = "";
    if (c->cost_usd > 0)
        snprintf(cost, sizeof cost, "$%.2f", c->cost_usd);

    if (!c->tokens_in && !c->tokens_out) {
        if (cost[0])
            note_line(notes, text_dsprintf("  %-8s %s", "spend", cost));
        return;
    }

    char in[32], out[32];
    text_humanize(c->tokens_in, in, sizeof in);
    text_humanize(c->tokens_out, out, sizeof out);
    note_line(notes, text_dsprintf("  %-8s %s in · %s out%s%s", "spend", in,
                                   out, cost[0] ? " · " : "", cost));
}

#define NOTE_LEAD 14 /* the width of "HH:MM  who    " */

/* Strip escapes and control characters, keeping the line structure: the card
 * shows a whole turn, and its paragraphs and lists are the formatting. */
static char *clean(const char *text)
{
    size_t n = strlen(text);
    char  *out = malloc(n + 1);
    if (!out)
        return NULL;

    size_t w = 0;
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t           end = ui_esc_span(text, n, i, &kind);
        if (kind == UI_ESC_TEXT)
            for (size_t k = i; k < end; k++) {
                unsigned char ch = (unsigned char)text[k];
                if (ch == '\r')
                    continue;
                if (ch == '\t')
                    ch = ' ';
                if (ch != '\n' && ch < ' ')
                    continue;
                if (ch == 0x7f)
                    continue;
                if (ch == ' ' && w && out[w - 1] == '\n')
                    continue;
                out[w++] = (char)ch;
            }
        i = end;
    }
    while (w && (out[w - 1] == '\n' || out[w - 1] == ' '))
        w--;
    out[w] = '\0';
    return out;
}

/* The first line carries the stamp and who said it; the rest sit under them. */
static void note_entry(struct notes *notes, const char *stamp, const char *who,
                       const char *text)
{
    const char *at = text;
    int         first = 1;

    do {
        const char *nl = strchr(at, '\n');
        size_t      len = nl ? (size_t)(nl - at) : strlen(at);

        if (first)
            note_line(notes, text_dsprintf("%s  %-6s %.*s", stamp, who,
                                           (int)len, at));
        else if (len)
            note_line(notes, text_dsprintf("%*s%.*s", NOTE_LEAD, "",
                                           (int)len, at));
        else
            note_line(notes, NULL);

        first = 0;
        at = nl ? nl + 1 : NULL;
    } while (at);
}

/* A worker's final message is a whole turn, not a lifecycle line: the stamp
   stands alone and the message sits under it at the card's own indent. */
static void note_message(struct notes *notes, const char *stamp,
                         const char *who, const char *text)
{
    note_line(notes, text_dsprintf("%s  %s", stamp, who));

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

static void build_proposals(const struct board_card *c, struct notes *notes)
{
    char line[BOARD_TITLE_MAX];
    note_line(notes, text_dsprintf("  proposals"));
    for (int i = 0; boardsweep_proposal(c, i, line, sizeof line); i++)
        note_line(notes, text_dsprintf("  %2d. %s", i + 1, line));
    note_line(notes, NULL);
}

static void build_notes(const struct board_card *c, struct notes *notes)
{
    if (boardflow_waits_on_you(c) && boardsweep_is(c))
        build_proposals(c, notes);
    build_stages(c, notes);
    build_backend(c, notes);
    build_spend(c, notes);

    for (int i = 0; i < c->log_n; i++) {
        if (i == 0)
            note_line(notes, NULL);
        struct tm when;
        char      stamp[16] = "     ";
        if (c->log[i].ts) {
            localtime_r(&c->log[i].ts, &when);
            strftime(stamp, sizeof stamp, "%H:%M", &when);
        }
        char *text = clean(c->log[i].text ? c->log[i].text : "");
        if (text && strchr(text, '\n'))
            note_message(notes, stamp, c->log[i].who, text);
        else
            note_entry(notes, stamp, c->log[i].who, text ? text : "");
        free(text);
    }
}

enum boardcard_action boardcard_form(const struct board_card *c)
{
    const char        *cols[BOARD_COLS + STEPS_SHOWN];
    const char *const *walk = NULL;
    int                walk_n = boardcfg_steps(&walk);
    int                cols_n = 0;
    for (int i = 0; i < BOARD_COLS; i++) {
        if (i == BOARD_STEP) {
            for (int j = 0; j < walk_n && j < STEPS_SHOWN; j++)
                cols[cols_n++] = walk[j];
            continue;
        }
        cols[cols_n++] = board_col_name((enum board_col)i);
    }

    const struct board_cfg *cfg = boardcfg();
    const char             *kinds[BOARD_KINDS_MAX + 1];
    int                     kinds_n = 0;
    kinds[kinds_n++] = "";
    for (int i = 0; i < cfg->kinds_n; i++)
        kinds[kinds_n++] = cfg->kinds[i].name;

    const char *backends[BOARD_BACKENDS_MAX + 1];
    int         backends_n = boardcfg_backend_choices(cfg, backends);
    const char *tiers[BOARD_TIERS + 1];
    int         tiers_n = boardcfg_tier_choices(tiers);

    char spec[8192];
    char kind[16];
    char column[16];
    char where[4096];
    char priority[8];
    char backend[32];
    char tier[8];

    snprintf(spec, sizeof spec, "%s", c->body ? c->body : "");
    snprintf(kind, sizeof kind, "%s", c->kind);
    snprintf(column, sizeof column, "%s", board_where(c));
    path_home_relative(c->cwd, where, sizeof where);
    snprintf(priority, sizeof priority, "%d", c->priority);
    snprintf(backend, sizeof backend, "%s", c->backend_pin);
    snprintf(tier, sizeof tier, "%s", c->tier_pin);

    char unstart_at[2] = "";
    char approve_at[2] = "";

    int proposals = boardflow_waits_on_you(c) && boardsweep_is(c);

    struct form_field fields[9];
    int               fields_n = 0;

    if (!proposals)
        fields[fields_n++] = (struct form_field){"spec", FORM_TEXT, spec,
                                                 sizeof spec, NULL, 0};
    fields[fields_n++] = (struct form_field){"kind", FORM_CHOICE, kind,
                                             sizeof kind, kinds, kinds_n};
    fields[fields_n++] = (struct form_field){"column", FORM_CHOICE, column,
                                             sizeof column, cols, cols_n};
    fields[fields_n++] = (struct form_field){"repo", FORM_TEXT, where,
                                             sizeof where, NULL, 0};
    fields[fields_n++] = (struct form_field){"priority", FORM_CHOICE, priority,
                                             sizeof priority, PRIORITIES,
                                             COUNT(PRIORITIES)};
    fields[fields_n++] = (struct form_field){"backend", FORM_CHOICE, backend,
                                             sizeof backend, backends,
                                             backends_n};
    fields[fields_n++] = (struct form_field){"tier", FORM_CHOICE, tier,
                                             sizeof tier, tiers, tiers_n};
    if (boardflow_runs(c) == BOARD_RUNS_WORKER && !boardsweep_is(c))
        fields[fields_n++] = (struct form_field){
            "cancel starting, back to backlog", FORM_BUTTON, unstart_at,
            sizeof unstart_at, NULL, 0};
    const struct board_role *waiting = boardflow_role(c);
    char approve_label[96];
    snprintf(approve_label, sizeof approve_label, "%s",
             waiting ? waiting->pass_label : "approve");
    if (waiting) {
        const char *next = boardflow_next(c, c->step, 0);
        const char *most = boardflow_next(c, c->step, 1);
        if (most && (!next || strcmp(next, most)))
            snprintf(approve_label + strlen(approve_label),
                     sizeof approve_label - strlen(approve_label),
                     " \xc2\xb7 no %s", most);
    }
    if (boardflow_waits_on_you(c) && boardplan_is(c))
        snprintf(approve_label, sizeof approve_label, "approve \xc2\xb7 file a card");
    if (proposals) {
        int raised = boardsweep_proposed(c);
        snprintf(approve_label, sizeof approve_label,
                 "approve · raise %d card%s", raised, raised == 1 ? "" : "s");
    }
    if (boardflow_waits_on_you(c))
        fields[fields_n++] = (struct form_field){approve_label, FORM_BUTTON,
                                                 approve_at, sizeof approve_at,
                                                 NULL, 0};

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
        .note_roles = notes.roles,
        .notes_n = notes.n,
        .fields = fields,
        .fields_n = fields_n,
    };
    int kept = form_run(&f);

    notes_free(&notes);
    if (unstart_at[0])
        return BOARDCARD_UNSTART;
    if (approve_at[0])
        return BOARDCARD_APPROVE;
    if (!kept)
        return BOARDCARD_NONE;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *live = board_find(cards, n, c->id);
    if (!live) {
        board_free(cards, n);
        return BOARDCARD_NONE;
    }

    struct board_card edited = *live;
    snprintf(edited.kind, sizeof edited.kind, "%s", kind);
    snprintf(edited.backend_pin, sizeof edited.backend_pin, "%s", backend);
    snprintf(edited.tier_pin, sizeof edited.tier_pin, "%s", tier);
    board_put(&edited, column);
    edited.priority = atoi(priority);

    int respec = !proposals && strcmp(spec, live->body ? live->body : "") != 0;
    if (!proposals) {
        edited.body = spec;
        if (!live->kind[0])
            board_title_of(spec, edited.title, sizeof edited.title);
    }

    /* a card the worker planned but that was not sorted as a plan: correcting
       the kind takes what the worker answered as the plan, so approving it
       files a card the way a plan does */
    const char *planned = NULL;
    if (!proposals && !respec && !boardplan_is(live) && boardplan_named(kind))
        planned = boardplan_said(live);
    if (planned)
        edited.body = (char *)planned;

    char *full = path_expand_home(where);
    if (full && *full)
        snprintf(edited.cwd, sizeof edited.cwd, "%s", full);
    free(full);

    int answered = respec && live->col == BOARD_UNCLEAR &&
                   edited.col == BOARD_UNCLEAR;
    if (answered)
        edited.col = BOARD_NEW;

    int repin = strcmp(edited.backend_pin, live->backend_pin) != 0 ||
                strcmp(edited.tier_pin, live->tier_pin) != 0;
    int adopted = planned != NULL;
    int ok = board_update(&edited);
    board_free(cards, n);

    if (ok && adopted)
        board_note(c->id, "you", "kind corrected to plan; the spec is now what "
                                 "the worker planned");

    if (ok && repin) {
        int waiting = 0;
        boardwork_serve(&waiting);
    }

    if (ok && answered) {
        board_note(c->id, "you", "spec edited; re-triaging");
        struct board_card *again = NULL;
        int                m = board_load(&again);
        struct board_card *fresh = board_find(again, m, c->id);
        if (fresh)
            boardtriage_start(fresh);
        board_free(again, m);
    }

    return BOARDCARD_NONE;
}
