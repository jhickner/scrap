#include "app.h"
#include "boardview.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ask.h"
#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "edit.h"
#include "boardaudit.h"
#include "boardcfgui.h"
#include "boardmerge.h"
#include "boardsweep.h"
#include "boardtriage.h"
#include "boardwork.h"
#include "child.h"
#include "chrome.h"
#include "confirm.h"
#include "form.h"
#include "pick.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"
#include "session.h"
#include "workspace.h"

#define KEY_NEW      'n'
#define KEY_DELETE   'd'
#define KEY_TRIAGE   't'
#define KEY_START    's'
#define KEY_GO       'g'
#define KEY_APPROVE  'a'
#define KEY_REJECT   'r'
#define KEY_FEEDBACK 'f'
#define KEY_LOG      'l'
#define KEY_CONFIG   'c'
#define KEY_ALL      '*'

#define BOARD_KEYS "ndtsgarflc*"

#define BOARD_RECENT 3

#define BOARD_RECENT_INDENT 6

#define BOARD_HINT \
    "enter edit  ·  s start  ·  g worker  ·  "                                \
    "a approve  ·  f feedback  ·  r reject\n"                                 \
    "n new  ·  t triage  ·  l log  ·  d delete  ·  c config  ·  "               \
    "* all repos  ·  / search"

struct vrow {
    char          id[BOARD_ID_MAX];
    char         *label;
    char         *detail;
    unsigned char heading;
    unsigned char spin;
    const char   *mark;
    unsigned char mark_role;
    int           action;
};

struct vlist {
    struct vrow *v;
    int          n, cap;
};

__attribute__((format(printf, 1, 2)))
static char *dsprintf(const char *fmt, ...)
{
    char    buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *out = strdup(buf);
    return out;
}

static struct vrow *row_add(struct vlist *l)
{
    if (l->n == l->cap) {
        int          cap = l->cap ? l->cap * 2 : 32;
        struct vrow *grown = realloc(l->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        l->v = grown;
        l->cap = cap;
    }
    struct vrow *r = &l->v[l->n];
    memset(r, 0, sizeof *r);
    l->n++;
    return r;
}

static void row_heading(struct vlist *l, const char *text)
{
    struct vrow *r = row_add(l);
    if (r) {
        r->label = dsprintf("%s", text);
        r->heading = PICK_HEADING;
    }
}

static int is_text(const struct vrow *r)
{
    return r->heading == PICK_HEADING || r->heading == PICK_TEXT;
}

static void vlist_free(struct vlist *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->v[i].label);
        free(l->v[i].detail);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int vlist_run(const char *title, struct vlist *l, int initial,
                     const char *hint, const char *shortcuts, int *pressed,
                     int (*tick)(void *ud), void *tick_ud, int *cursor)
{
    struct pick_item *items = calloc((size_t)l->n, sizeof *items);
    unsigned char    *heading = calloc((size_t)l->n, 1);
    unsigned char    *spin = calloc((size_t)l->n, 1);
    const char      **mark = calloc((size_t)l->n, sizeof *mark);
    unsigned char    *role = calloc((size_t)l->n, 1);
    if (!items || !heading || !spin || !mark || !role) {
        free(items);
        free(heading);
        free(spin);
        free(mark);
        free(role);
        return -1;
    }

    for (int i = 0; i < l->n; i++) {
        items[i].label = l->v[i].label ? l->v[i].label : "";
        items[i].detail = l->v[i].detail;
        heading[i] = l->v[i].heading;
        spin[i] = l->v[i].spin;
        mark[i] = l->v[i].mark;
        role[i] = l->v[i].mark_role;
    }

    struct pick_live live = {
        .heading = heading,
        .spin = spin,
        .mark = mark,
        .mark_role = role,
        .hint = hint,
        .align = 1,
        .tick = tick,
        .ud = tick_ud,
        .cursor = cursor,
        .keep = 1,
    };
    int at = pick_run_live(title, items, l->n, initial, &live, PICK_SEARCH_SLASH,
                           shortcuts, pressed);

    free(items);
    free(heading);
    free(spin);
    free(mark);
    free(role);
    return at;
}

static void ago(time_t then, char *out, size_t size)
{
    long gap = (long)(time(NULL) - then);
    if (gap < 60)
        snprintf(out, size, "just now");
    else if (gap < 3600)
        snprintf(out, size, "%ldm", gap / 60);
    else if (gap < 86400)
        snprintf(out, size, "%ldh", gap / 3600);
    else
        snprintf(out, size, "%ldd", gap / 86400);
}

static void column_mark(enum board_col col, const char **mark, unsigned char *role)
{
    switch (col) {
    case BOARD_UNCLEAR:
        *mark = "?";
        *role = UI_ACCENT;
        break;
    case BOARD_BACKLOG:
        *mark = "●";
        *role = UI_DIM;
        break;
    case BOARD_REVIEW:
        *mark = "✓";
        *role = UI_OK;
        break;
    case BOARD_DONE:
        *mark = "✓";
        *role = UI_DIM;
        break;
    default:
        *mark = NULL;
        *role = UI_DIM;
        break;
    }
}

#define WHERE_MAX 26

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

static const char *asked(const struct board_card *c)
{
    for (int i = c->log_n - 1; i >= 0; i--)
        if (!strcmp(c->log[i].who, "triage") && c->log[i].text)
            return c->log[i].text;
    return NULL;
}

static int shows(const struct board_card *c, const char *filter)
{
    return !filter || !*filter || !strcmp(c->cwd, filter);
}

static const char *step_of(const char *id)
{
    if (boardtriage_running(id))
        return "triaging";
    if (boardmerge_running(id))
        return "landing";
    if (boardwork_auditing(id))
        return "auditing";
    if (boardwork_sweeping(id))
        return "sweeping";
    return NULL;
}

static int by_priority(const void *a, const void *b)
{
    const struct board_card *const *x = a, *const *y = b;
    if ((*x)->priority != (*y)->priority)
        return (*y)->priority - (*x)->priority;
    if ((*x)->created != (*y)->created)
        return (*x)->created < (*y)->created ? -1 : 1;
    return strcmp((*x)->id, (*y)->id);
}

static int build_board(struct vlist *l, struct board_card *cards, int n,
                       const char *filter, int wide)
{
    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    if (!in)
        return 0;
    int shown = 0;

    for (int col = 0; col < BOARD_COLS; col++) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if ((int)cards[i].col == col && shows(&cards[i], filter))
                in[k++] = &cards[i];
        if (!k)
            continue;
        qsort(in, (size_t)k, sizeof *in, by_priority);

        shown += k;
        row_heading(l, board_col_name((enum board_col)col));
        for (int i = 0; i < k; i++) {
            const struct board_card *c = in[i];
            struct vrow             *r = row_add(l);
            if (!r)
                break;
            snprintf(r->id, sizeof r->id, "%s", c->id);
            r->label = dsprintf("%s", c->title[0] ? c->title : "(untitled)");
            column_mark(c->col, &r->mark, &r->mark_role);
            int         tab = boardwork_tab(c->id);
            const char *step = step_of(c->id);
            r->spin = (unsigned char)(step ||
                                      (c->col == BOARD_DOING && tab >= 0 &&
                                       session_busy(workspace_at(tab))));

            char ts[32], when[64];
            ago(c->updated ? c->updated : c->created, ts, sizeof ts);
            if (step && tab >= 0)
                snprintf(when, sizeof when, "%s · tab %d", step, tab + 1);
            else if (step)
                snprintf(when, sizeof when, "%s…", step);
            else if (tab >= 0)
                snprintf(when, sizeof when, "tab %d · %s", tab + 1, ts);
            else
                snprintf(when, sizeof when, "%s", ts);

            char where[256] = {0};
            if (wide)
                short_repo(c->cwd, where, sizeof where);

            char waiting[256] = {0};
            if (c->col == BOARD_BACKLOG)
                boardwork_blocked(c, waiting, sizeof waiting);

            const char *question = c->col == BOARD_UNCLEAR ? asked(c) : NULL;
            if (question)
                r->detail = dsprintf("%s", question);

            else if (c->stuck[0])
                r->detail = dsprintf("stuck · %s", c->stuck);
            else if (waiting[0])
                r->detail = dsprintf("waiting · %s", waiting);
            else if (c->col == BOARD_REVIEW && boardsweep_is(c)) {
                int raised = boardsweep_proposed(c);
                r->detail = dsprintf("%d card%s proposed", raised,
                                     raised == 1 ? "" : "s");
            } else if (c->col == BOARD_REVIEW && c->worktree[0]) {
                int files = 0, lines = 0;
                boardaudit_size(c, &files, &lines);
                r->detail = dsprintf("%d file%s, %d line%s", files,
                                     files == 1 ? "" : "s", lines,
                                     lines == 1 ? "" : "s");
            } else if (c->cost_usd > 0 &&
                     (c->col == BOARD_REVIEW || c->col == BOARD_DONE)) {
                char head[320] = "";
                if (where[0] && c->kind[0])
                    snprintf(head, sizeof head, "%s · %s · ", where, c->kind);
                else if (where[0])
                    snprintf(head, sizeof head, "%s · ", where);
                else if (c->kind[0])
                    snprintf(head, sizeof head, "%s · ", c->kind);
                r->detail = dsprintf("%s$%.2f · %s", head, c->cost_usd, when);
            }
            else if (where[0] && c->kind[0])
                r->detail = dsprintf("%s · %s · %s", where, c->kind, when);
            else if (where[0])
                r->detail = dsprintf("%s · %s", where, when);
            else if (c->kind[0])
                r->detail = dsprintf("%s · %s", c->kind, when);
            else
                r->detail = dsprintf("%s", when);

            if (tab >= 0) {
                const char *said[BOARD_RECENT];
                int         k = session_recent(workspace_at(tab), said, BOARD_RECENT);
                for (int j = 0; j < k; j++) {
                    struct vrow *t = row_add(l);
                    if (!t)
                        break;
                    t->heading = PICK_TEXT;
                    t->label = dsprintf("%*s%s", BOARD_RECENT_INDENT, "", said[j]);
                }
            }
        }
    }
    free(in);
    return shown;
}

static int row_of(const struct vlist *l, const char *id)
{
    if (id && *id)
        for (int i = 0; i < l->n; i++)
            if (!is_text(&l->v[i]) && !strcmp(l->v[i].id, id))
                return i;
    for (int i = 0; i < l->n; i++)
        if (!is_text(&l->v[i]))
            return i;
    return 0;
}

static int board_reap(void)
{
    int  changed = 0;
    char key[CHILD_KEY_MAX];

    for (;;) {
        char *out = NULL;
        int   ok = 0;
        if (!child_reap(key, sizeof key, &out, &ok))
            break;
        changed |= boardtriage_take(key, out);
        changed |= boardmerge_take(key, out, ok);
        free(out);
    }
    return changed;
}

static unsigned long shown_rev;

static int board_tick(void *ud)
{
    (void)ud;

    workspace_pump_quiet();

    int moved = board_reap();
    moved |= boardwork_poll();
    moved |= boardwork_pump();

    moved |= boardmerge_pump();
    moved |= boardwork_audit_pump();
    moved |= boardwork_sweep_pump();

    static int seen;
    int        now = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++)
        now += session_recent_seq(workspace_at(i));
    if (now != seen) {
        seen = now;
        moved = 1;
    }

    static unsigned seen_busy;
    unsigned        busy = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++)
        if (session_busy(workspace_at(i)))
            busy |= 1u << i;
    if (busy != seen_busy) {
        seen_busy = busy;
        moved = 1;
    }

    if (board_revision() != shown_rev)
        moved = 1;

    return moved ? PICK_TICK_REOPEN : 0;
}

static void triage_the_new(struct board_card *cards, int n)
{
    for (int i = 0; i < n; i++)
        if (cards[i].col == BOARD_NEW && !boardtriage_running(cards[i].id) &&
            boardtriage_attempts(&cards[i]) == 0)
            boardtriage_start(&cards[i]);
}

__attribute__((format(printf, 1, 2)))
static void note(const char *fmt, ...)
{
    char    text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", text);
    viewport_item_end();
    ui_flush();
}

static void do_new(const char *cwd, char *sel_id)
{
    char *text = ask_run("new card", NULL);
    if (!text)
        return;
    char id[BOARD_ID_MAX] = {0};
    if (board_add(text, cwd, id))
        snprintf(sel_id, BOARD_ID_MAX, "%s", id);
    free(text);
}

static int do_delete(const struct board_card *c)
{
    char question[280];
    snprintf(question, sizeof question, "delete \"%s\"?", c->title);
    if (!confirm_run(question))
        return 0;
    boardwork_discard(c);
    return board_remove(c->id);
}

static const char *const PRIORITIES[] = {"0", "1", "2", "3"};

#define NOTES_MAX 48

static void note_line(const char **notes, int *n, char **owned, const char *text)
{
    if (*n >= NOTES_MAX)
        return;
    char *copy = dsprintf("%s", text);
    owned[*n] = copy;
    notes[*n] = copy ? copy : "";
    (*n)++;
}

static int stage_ran(const struct board_card *c, const char *who)
{
    for (int i = 0; i < c->log_n; i++)
        if (!strcmp(c->log[i].who, who))
            return 1;
    return 0;
}

static void build_stages(const struct board_card *c, const char **notes, int *n,
                         char **owned)
{
    static const struct {
        const char     *name;
        enum board_col  at;
        enum board_step step;
        const char     *who;
    } STAGE[] = {
        {"triage", BOARD_NEW,     BOARD_STEPS,       "triage"},
        {"worker", BOARD_DOING,   BOARD_STEPS,       "worker"},
        {"review", BOARD_REVIEW,  BOARD_STEP_REVIEW, "you"},
        {"audit",  BOARD_AUDIT,   BOARD_STEP_AUDIT,  "audit"},
        {"merge",  BOARD_MERGING, BOARD_STEP_MERGE,  NULL},
    };

    for (size_t i = 0; i < sizeof STAGE / sizeof *STAGE; i++) {
        if (STAGE[i].step != BOARD_STEPS &&
            !boardcfg_kind_takes(c->kind, STAGE[i].step))
            continue;
        if (boardsweep_is(c) && STAGE[i].at != BOARD_DOING &&
            STAGE[i].at != BOARD_REVIEW)
            continue;

        const char *state;
        int         here = c->col == STAGE[i].at ||
                           (STAGE[i].at == BOARD_NEW && c->col == BOARD_UNCLEAR);
        if (here)
            state = "current";
        else if (c->col < STAGE[i].at)
            state = "pending";
        else if (STAGE[i].who && !stage_ran(c, STAGE[i].who))
            state = "skipped";
        else
            state = "done";

        char *line = dsprintf("  %-8s %s", STAGE[i].name, state);
        note_line(notes, n, owned, line ? line : "");
        free(line);
    }
}

static int build_notes(const struct board_card *c, const char **notes, char **owned)
{
    int n = 0;

    build_stages(c, notes, &n, owned);

    for (int i = 0; i < c->log_n && n < NOTES_MAX; i++) {
        if (i == 0)
            note_line(notes, &n, owned, "");
        struct tm when;
        char      stamp[16] = "     ";
        if (c->log[i].ts) {
            localtime_r(&c->log[i].ts, &when);
            strftime(stamp, sizeof stamp, "%H:%M", &when);
        }
        char *line = dsprintf("%s  %-6s %s", stamp, c->log[i].who,
                              c->log[i].text ? c->log[i].text : "");
        note_line(notes, &n, owned, line ? line : "");
        free(line);
    }
    return n;
}

static void card_form(const struct board_card *c)
{
    const char *cols[BOARD_COLS];
    for (int i = 0; i < BOARD_COLS; i++)
        cols[i] = board_col_name((enum board_col)i);

    const struct board_cfg *cfg = boardcfg();
    const char             *kinds[BOARD_KINDS_MAX + 1];
    int                     kinds_n = 0;
    kinds[kinds_n++] = "";
    for (int i = 0; i < cfg->kinds_n; i++)
        kinds[kinds_n++] = cfg->kinds[i].name;

    char spec[8192];
    char kind[16];
    char column[16];
    char where[4096];
    char priority[8];

    snprintf(spec, sizeof spec, "%s", c->body ? c->body : "");
    snprintf(kind, sizeof kind, "%s", c->kind);
    snprintf(column, sizeof column, "%s", board_col_name(c->col));
    path_home_relative(c->cwd, where, sizeof where);
    snprintf(priority, sizeof priority, "%d", c->priority);

    struct form_field fields[6];
    int               fields_n = 0;

    fields[fields_n++] = (struct form_field){"spec", FORM_TEXT, spec,
                                             sizeof spec, NULL, 0};
    fields[fields_n++] = (struct form_field){"kind", FORM_CHOICE, kind,
                                             sizeof kind, kinds, kinds_n};
    fields[fields_n++] = (struct form_field){"column", FORM_CHOICE, column,
                                             sizeof column, cols, BOARD_COLS};
    fields[fields_n++] = (struct form_field){"repo", FORM_TEXT, where,
                                             sizeof where, NULL, 0};
    fields[fields_n++] = (struct form_field){"priority", FORM_CHOICE, priority,
                                             sizeof priority, PRIORITIES,
                                             COUNT(PRIORITIES)};

    const char *notes[NOTES_MAX] = {0};
    char       *owned[NOTES_MAX] = {0};
    int         notes_n = build_notes(c, notes, owned);

    char heading[600];
    if (c->kind[0] && c->title[0] && strcmp(c->title, c->body ? c->body : ""))
        snprintf(heading, sizeof heading, "%s · %s · %s", c->id, c->kind, c->title);
    else
        snprintf(heading, sizeof heading, "%s · %s", c->id,
                 c->kind[0] ? c->kind : "unsorted");

    struct form f = {
        .title = heading,
        .notes = notes,
        .notes_n = notes_n,
        .fields = fields,
        .fields_n = fields_n,
    };
    int kept = form_run(&f);

    for (int i = 0; i < notes_n; i++)
        free(owned[i]);
    if (!kept)
        return;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *live = board_find(cards, n, c->id);
    if (!live) {
        board_free(cards, n);
        return;
    }

    struct board_card edited = *live;
    snprintf(edited.kind, sizeof edited.kind, "%s", kind);
    edited.col = board_col_from_name(column);
    edited.priority = atoi(priority);

    int respec = strcmp(spec, live->body ? live->body : "") != 0;
    edited.body = spec;
    if (!live->kind[0])
        board_title_of(spec, edited.title, sizeof edited.title);

    char *full = path_expand_home(where);
    if (full && *full)
        snprintf(edited.cwd, sizeof edited.cwd, "%s", full);
    free(full);

    int answered = respec && live->col == BOARD_UNCLEAR &&
                   edited.col == BOARD_UNCLEAR;
    if (answered)
        edited.col = BOARD_NEW;

    int ok = board_update(&edited);
    board_free(cards, n);

    if (ok && answered) {
        board_note(c->id, "you", "spec edited; re-triaging");
        struct board_card *again = NULL;
        int                m = board_load(&again);
        struct board_card *fresh = board_find(again, m, c->id);
        if (fresh)
            boardtriage_start(fresh);
        board_free(again, m);
    }
}

int boardview_capture(const char *text, const char *cwd, char *id_out, int size)
{
    char id[BOARD_ID_MAX] = {0};
    if (!board_add(text, cwd, id))
        return 0;
    if (id_out && size > 0)
        snprintf(id_out, (size_t)size, "%s", id);
    return 1;
}

static void close_list(void)
{
    chrome_modal(NULL, NULL);
}

int boardview_run(const char *cwd)
{
    char here[4096];
    snprintf(here, sizeof here, "%s", cwd ? cwd : "");

    static char filter[4096];
    static char sel_id[BOARD_ID_MAX];
    char        notice[256] = {0};
    static int  been_here;

    if (!been_here) {
        snprintf(filter, sizeof filter, "%s", here);
        been_here = 1;
    }

    for (;;) {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);

        triage_the_new(cards, n);

        if (board_archive(boardcfg()->archive_after)) {
            board_free(cards, n);
            n = board_load(&cards);
        }

        shown_rev = board_revision();

        struct vlist l = {0};
        int          shown = build_board(&l, cards, n, filter, !filter[0]);

        if (!l.n) {
            vlist_free(&l);
            board_free(cards, n);

            if (filter[0]) {
                filter[0] = '\0';
                continue;
            }
            close_list();
            note("the board is empty — /card <text> puts something on it");
            return -1;
        }

        char where[512] = "all repos";
        if (filter[0])
            path_home_relative(filter, where, sizeof where);

        const struct board_cfg *cfg = boardcfg();
        char busy[64] = "";
        if (boardwork_running())
            snprintf(busy, sizeof busy, " · %d/%d workers", boardwork_running(),
                     cfg->workers);

        double spent = 0;
        for (int i = 0; i < n; i++)
            if (!filter[0] || !strcmp(cards[i].cwd, filter))
                spent += cards[i].cost_usd;

        char left[64] = "";
        if (spent > 0)
            snprintf(left, sizeof left, " · $%.2f", spent);

        char title[820];
        if (shown == n)
            snprintf(title, sizeof title, "board · %s · %d card%s%s%s", where, n,
                     n == 1 ? "" : "s", busy, left);
        else
            snprintf(title, sizeof title, "board · %s · %d of %d%s%s", where,
                     shown, n, busy, left);

        char hint[512];
        if (notice[0])
            snprintf(hint, sizeof hint, "%s\n%s", notice, BOARD_HINT);
        else
            snprintf(hint, sizeof hint, "%s", BOARD_HINT);
        notice[0] = '\0';

        int pressed = 0;
        int cursor = -1;
        int at = vlist_run(title, &l, row_of(&l, sel_id), hint, BOARD_KEYS,
                           &pressed, board_tick, NULL, &cursor);
        if (at >= 0)
            snprintf(sel_id, sizeof sel_id, "%s", l.v[at].id);
        else if (cursor >= 0)
            snprintf(sel_id, sizeof sel_id, "%s", l.v[cursor].id);
        vlist_free(&l);

        if (at == PICK_REOPEN) {
            board_free(cards, n);
            continue;
        }
        if (at < 0) {
            close_list();
            board_free(cards, n);
            return -1;
        }

        struct board_card *c = board_find(cards, n, sel_id);
        switch (pressed) {
        case 0:
            if (c) {
                close_list();
                card_form(c);
            }
            break;
        case KEY_NEW:
            close_list();
            do_new(filter[0] ? filter : here, sel_id);
            break;
        case KEY_TRIAGE:
            if (c)
                boardtriage_start(c);
            break;
        case KEY_START:
            if (c) {
                char why[256];
                if (!boardwork_start(c, why, sizeof why))
                    snprintf(notice, sizeof notice, "%s", why);
            }
            break;
        case KEY_GO: {
            int tab = c ? boardwork_tab(c->id) : -1;
            if (tab >= 0) {
                close_list();
                board_free(cards, n);
                return tab;
            }
            if (c) {
                close_list();
                const char *step = step_of(c->id);
                if (step)
                    note("%s in the background — no tab", step);
                else
                    note("no worker has that card");
            }
            break;
        }
        case KEY_APPROVE:
            if (c && c->col == BOARD_REVIEW && boardsweep_is(c)) {
                boardwork_let_go(c->id);
                boardsweep_approve(c);
            } else if (c && c->col == BOARD_REVIEW) {
                close_list();
                int files = 0, lines = 0;
                boardaudit_size(c, &files, &lines);
                char ask[256];
                snprintf(ask, sizeof ask, "audit %d file%s, %d line%s before it lands?",
                         files, files == 1 ? "" : "s", lines, lines == 1 ? "" : "s");
                boardwork_approve(c, confirm_run(ask));
            }
            break;
        case KEY_REJECT:
            if (c && boardsweep_is(c)) {
                boardwork_let_go(c->id);
                boardsweep_reject(c);
            } else if (c && (c->col == BOARD_REVIEW || c->col == BOARD_DOING)) {
                close_list();
                char *why = ask_run("why is it going back?", NULL);
                if (why) {
                    boardwork_reject(c, why);
                    free(why);
                }
            }
            break;
        case KEY_FEEDBACK:
            if (c && boardwork_tab(c->id) >= 0) {
                close_list();
                char *say = ask_run("what should it do?", NULL);
                if (say) {
                    if (!boardwork_feedback(c, say))
                        note("the worker did not take it");
                    free(say);
                }
            }
            break;
        case KEY_DELETE:
            if (c) {
                close_list();
                if (do_delete(c))
                    sel_id[0] = '\0';
            }
            break;
        case KEY_LOG: {
            char path[4300];
            if (c) {
                close_list();
                if (boardlog_path(c->id, path, sizeof path) && !edit_open(path))
                    note("nothing has happened to this card yet");
            }
            break;
        }
        case KEY_CONFIG:
            close_list();
            boardcfgui_run();
            break;
        case KEY_ALL:
            if (filter[0])
                filter[0] = '\0';
            else
                snprintf(filter, sizeof filter, "%s", here);
            break;
        default:
            break;
        }
        board_free(cards, n);
    }
}
