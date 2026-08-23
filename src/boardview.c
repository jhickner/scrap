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
#include "boardundo.h"
#include "boardwork.h"
#include "child.h"
#include "chrome.h"
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
#define KEY_START_MAX 'S'
#define KEY_GO       'g'
#define KEY_APPROVE  'a'
#define KEY_APPROVE_ALL 'A'
#define KEY_AUDIT    'i'
#define KEY_SKIP     'k'
#define KEY_UNDO     'u'
#define KEY_REJECT   'r'
#define KEY_UNSTART  'x'
#define KEY_FEEDBACK 'f'
#define KEY_LOG      'l'
#define KEY_CONFIG   'c'
#define KEY_SERVE    'b'
#define KEY_ALL      '*'

#define BOARD_KEYS "ndtsSgarxflcb*Aiuk\t"

#define BOARD_RECENT 3

#define BOARD_RECENT_INDENT 6

#define BOARD_HINT \
    "enter edit  ·  s start  ·  S start max  ·  g worker  ·  "                \
    "a approve  ·  A approve all  ·  i audit\n"                               \
    "f feedback  ·  r send back  ·  x cancel start  ·  u undo  ·  "          \
    "k skip audit\n"                                                          \
    "n new  ·  t triage  ·  l log  ·  d delete  ·  c config  ·  "               \
    "b backend  ·  * all repos  ·  / search"

struct vrow {
    char          id[BOARD_ID_MAX];
    unsigned char col;
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
                     const char *hint, const char *ask, const char *shortcuts,
                     int *pressed, int (*tick)(void *ud), void *tick_ud, int *cursor)
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
        .ask = ask,
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

static time_t stamp_due;

static void stamp_next(time_t then)
{
    long gap = (long)(time(NULL) - then);
    if (gap < 0)
        gap = 0;
    time_t next;
    if (gap < 3600)
        next = then + (gap / 60 + 1) * 60;
    else if (gap < 86400)
        next = then + (gap / 3600 + 1) * 3600;
    else
        next = then + (gap / 86400 + 1) * 86400;
    if (!stamp_due || next < stamp_due)
        stamp_due = next;
}

static void ago(time_t then, int done, char *out, size_t size)
{
    long gap = (long)(time(NULL) - then);
    if (gap < 60)
        snprintf(out, size, "just now");
    else if (gap < 3600)
        snprintf(out, size, "%ldm%s", gap / 60, done ? " ago" : "");
    else if (gap < 86400)
        snprintf(out, size, "%ldh%s", gap / 3600, done ? " ago" : "");
    else
        snprintf(out, size, "%ldd%s", gap / 86400, done ? " ago" : "");
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

static int by_col(const void *a, const void *b)
{
    const struct board_card *const *x = a, *const *y = b;
    return board_cmp_col(*x, *y);
}

static int build_board(struct vlist *l, struct board_card *cards, int n,
                       const char *filter, int wide)
{
    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    if (!in)
        return 0;
    int shown = 0;

    stamp_due = 0;

    for (int col = 0; col < BOARD_COLS; col++) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if ((int)cards[i].col == col && shows(&cards[i], filter))
                in[k++] = &cards[i];
        if (!k)
            continue;
        qsort(in, (size_t)k, sizeof *in, by_col);

        shown += k;
        row_heading(l, board_col_name((enum board_col)col));
        for (int i = 0; i < k; i++) {
            const struct board_card *c = in[i];
            struct vrow             *r = row_add(l);
            if (!r)
                break;
            snprintf(r->id, sizeof r->id, "%s", c->id);
            r->col = (unsigned char)c->col;
            r->label = dsprintf("%s", c->title[0] ? c->title : "(untitled)");
            column_mark(c->col, &r->mark, &r->mark_role);
            int         tab = boardwork_tab(c->id);
            const char *step = step_of(c->id);
            r->spin = (unsigned char)(step ||
                                      (c->col == BOARD_DOING && tab >= 0 &&
                                       session_busy(workspace_at(tab))));

            char ts[32], when[64];
            ago(c->updated ? c->updated : c->created, c->col == BOARD_DONE,
                 ts, sizeof ts);
            if (step && tab >= 0)
                snprintf(when, sizeof when, "%s · tab %d", step, tab + 1);
            else if (step)
                snprintf(when, sizeof when, "%s…", step);
            else {
                stamp_next(c->updated ? c->updated : c->created);
                if (tab >= 0)
                    snprintf(when, sizeof when, "tab %d · %s", tab + 1, ts);
                else
                    snprintf(when, sizeof when, "%s", ts);
            }

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

            if (c->backend_pin[0]) {
                char *was = r->detail;
                r->detail = dsprintf("%s%s%s", was ? was : "",
                                     was && *was ? " · " : "", c->backend_pin);
                free(was);
            }

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

static int find_id(const struct vlist *l, const char *id)
{
    if (id && *id)
        for (int i = 0; i < l->n; i++)
            if (!is_text(&l->v[i]) && !strcmp(l->v[i].id, id))
                return i;
    return -1;
}

/* col is the column the card sat in when the cursor last rested on it, or -1
   to follow that card wherever it has gone. */
struct anchor {
    char id[BOARD_ID_MAX];
    char next[BOARD_ID_MAX];
    char prev[BOARD_ID_MAX];
    int  col;
};

static void anchor_set(struct anchor *a, const struct vlist *l, int row, int col)
{
    snprintf(a->id, sizeof a->id, "%s", l->v[row].id);
    a->col = col;
    a->next[0] = a->prev[0] = '\0';
    for (int i = row + 1; i < l->n; i++)
        if (!is_text(&l->v[i])) {
            snprintf(a->next, sizeof a->next, "%s", l->v[i].id);
            break;
        }
    for (int i = row - 1; i >= 0; i--)
        if (!is_text(&l->v[i])) {
            snprintf(a->prev, sizeof a->prev, "%s", l->v[i].id);
            break;
        }
}

static int row_of(const struct vlist *l, const struct anchor *a)
{
    int at = find_id(l, a->id);
    if (at >= 0 && (a->col < 0 || (int)l->v[at].col == a->col))
        return at;

    int by = find_id(l, a->next);
    if (by < 0)
        by = find_id(l, a->prev);
    if (by >= 0)
        return by;
    if (at >= 0)
        return at;

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

    if (stamp_due && time(NULL) >= stamp_due)
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

static void do_serve(char *notice, size_t size)
{
    const struct board_cfg *cfg = boardcfg();

    struct pick_item items[BOARD_BACKENDS_MAX];
    char             details[BOARD_BACKENDS_MAX][160];
    int              at = 0;

    for (int i = 0; i < cfg->backends_n; i++) {
        const struct board_backend *b = &cfg->backends[i];
        size_t                      used = 0;
        details[i][0] = '\0';
        for (int t = 0; t < BOARD_TIERS; t++)
            used += (size_t)snprintf(details[i] + used, sizeof details[i] - used,
                                     "%s%s", t ? " · " : "",
                                     b->level[t].model[0] ? b->level[t].model
                                                          : "default");
        items[i] = (struct pick_item){b->name, details[i]};
        if (!strcmp(b->name, boardcfg_serving()))
            at = i;
    }
    if (!cfg->backends_n)
        return;

    int chosen = pick_run("serving the board", items, cfg->backends_n, at);
    if (chosen < 0)
        return;

    char name[32];
    snprintf(name, sizeof name, "%s", cfg->backends[chosen].name);
    if (!boardcfg_set_serving(name)) {
        snprintf(notice, size, "could not switch to %s", name);
        return;
    }

    int waiting = 0;
    int moved = boardwork_serve(&waiting);

    size_t used = (size_t)snprintf(notice, size, "serving %s", name);
    if (moved)
        used += (size_t)snprintf(notice + used, size - used,
                                 " · %d worker%s switched", moved,
                                 moved == 1 ? "" : "s");
    if (waiting)
        snprintf(notice + used, size - used, " · %d after their current card", waiting);
}

enum { ASK_NONE, ASK_APPROVE_ALL, ASK_DELETE };

static int do_delete(const struct board_card *c)
{
    boardwork_discard(c);
    return board_remove(c->id);
}

static void approve(const struct board_card *c, int audit)
{
    if (!c || c->col != BOARD_REVIEW)
        return;
    if (boardsweep_is(c)) {
        boardwork_let_go(c->id);
        boardsweep_approve(c);
    } else
        boardwork_approve(c, audit);
}

static int in_review(const struct board_card *cards, int n, const char *filter)
{
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (cards[i].col == BOARD_REVIEW && shows(&cards[i], filter))
            ready++;
    return ready;
}

static int approve_all(const struct board_card *cards, int n, const char *filter)
{
    int did = 0;
    for (int i = 0; i < n; i++) {
        if (cards[i].col != BOARD_REVIEW || !shows(&cards[i], filter))
            continue;
        approve(&cards[i], 0);
        did++;
    }
    return did;
}

static int in_backlog(const struct board_card *cards, int n, const char *filter)
{
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (cards[i].col == BOARD_BACKLOG && shows(&cards[i], filter))
            ready++;
    return ready;
}

static int start_max(const struct board_card *cards, int n, const char *filter,
                     char *why, int size)
{
    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    if (!in)
        return 0;

    int k = 0;
    for (int i = 0; i < n; i++)
        if (cards[i].col == BOARD_BACKLOG && shows(&cards[i], filter))
            in[k++] = &cards[i];
    qsort(in, (size_t)k, sizeof *in, by_col);

    int did = 0;
    if (why && size > 0)
        why[0] = '\0';
    for (int i = 0; i < k; i++) {
        if (boardwork_start(in[i], why, size)) {
            did++;
            if (why && size > 0)
                why[0] = '\0';
            continue;
        }
        if (boardwork_running() >= boardcfg()->workers ||
            workspace_count() >= WORKSPACE_MAX)
            break;
    }
    free(in);
    return did;
}

static const char *const PRIORITIES[] = {"0", "1", "2", "3"};

struct notes {
    const char **v;
    char       **owned;
    int          n, cap;
};

/* takes ownership of text */
static void note_line(struct notes *l, char *text)
{
    if (l->n == l->cap) {
        int          cap = l->cap ? l->cap * 2 : 32;
        const char **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (v)
            l->v = v;
        char **owned = realloc(l->owned, (size_t)cap * sizeof *owned);
        if (owned)
            l->owned = owned;
        if (!v || !owned) {
            free(text);
            return;
        }
        l->cap = cap;
    }
    l->owned[l->n] = text;
    l->v[l->n] = text ? text : "";
    l->n++;
}

static void notes_free(struct notes *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->owned[i]);
    free(l->owned);
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

static void build_stages(const struct board_card *c, struct notes *notes)
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

        note_line(notes, dsprintf("  %-8s %s", STAGE[i].name, state));
    }
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

    note_line(notes, dsprintf("  %-8s %s · %s", "backend", name,
                              s ? "working" : "worked"));
}

static void build_spend(const struct board_card *c, struct notes *notes)
{
    char cost[32] = "";
    if (c->cost_usd > 0)
        snprintf(cost, sizeof cost, "$%.2f", c->cost_usd);

    if (!c->tokens_in && !c->tokens_out) {
        if (cost[0])
            note_line(notes, dsprintf("  %-8s %s", "spend", cost));
        return;
    }

    char in[32], out[32];
    text_humanize(c->tokens_in, in, sizeof in);
    text_humanize(c->tokens_out, out, sizeof out);
    note_line(notes, dsprintf("  %-8s %s in · %s out%s%s", "spend", in, out,
                              cost[0] ? " · " : "", cost));
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
            note_line(notes, dsprintf("%s  %-6s %.*s", stamp, who, (int)len, at));
        else if (len)
            note_line(notes, dsprintf("%*s%.*s", NOTE_LEAD, "", (int)len, at));
        else
            note_line(notes, NULL);

        first = 0;
        at = nl ? nl + 1 : NULL;
    } while (at);
}

static void build_notes(const struct board_card *c, struct notes *notes)
{
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
        note_entry(notes, stamp, c->log[i].who, text ? text : "");
        free(text);
    }
}

static int unstart(const struct board_card *c)
{
    if (!c || c->col != BOARD_DOING || boardsweep_is(c))
        return 0;
    return boardwork_reject(c, "cancelled start");
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

    const char *backends[BOARD_BACKENDS_MAX + 1];
    int         backends_n = 0;
    backends[backends_n++] = "";
    for (int i = 0; i < cfg->backends_n; i++)
        backends[backends_n++] = cfg->backends[i].name;

    char spec[8192];
    char kind[16];
    char column[16];
    char where[4096];
    char priority[8];
    char backend[32];

    snprintf(spec, sizeof spec, "%s", c->body ? c->body : "");
    snprintf(kind, sizeof kind, "%s", c->kind);
    snprintf(column, sizeof column, "%s", board_col_name(c->col));
    path_home_relative(c->cwd, where, sizeof where);
    snprintf(priority, sizeof priority, "%d", c->priority);
    snprintf(backend, sizeof backend, "%s", c->backend_pin);

    char unstart_at[2] = "";
    char approve_at[2] = "";

    struct form_field fields[8];
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
    fields[fields_n++] = (struct form_field){"backend", FORM_CHOICE, backend,
                                             sizeof backend, backends,
                                             backends_n};
    if (c->col == BOARD_DOING && !boardsweep_is(c))
        fields[fields_n++] = (struct form_field){
            "cancel starting, back to backlog", FORM_BUTTON, unstart_at,
            sizeof unstart_at, NULL, 0};
    if (c->col == BOARD_REVIEW)
        fields[fields_n++] = (struct form_field){"approve", FORM_BUTTON,
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
        .notes_n = notes.n,
        .fields = fields,
        .fields_n = fields_n,
    };
    int kept = form_run(&f);

    notes_free(&notes);
    if (unstart_at[0] || approve_at[0]) {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *live = board_find(cards, n, c->id);
        if (live) {
            if (unstart_at[0])
                unstart(live);
            else
                approve(live, 0);
        }
        board_free(cards, n);
        return;
    }
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
    snprintf(edited.backend_pin, sizeof edited.backend_pin, "%s", backend);
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

    int repin = strcmp(edited.backend_pin, live->backend_pin) != 0;
    int ok = board_update(&edited);
    board_free(cards, n);

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
    boardcfg_reload();

    char why[4400];
    if (boardcfg_missing(why, sizeof why)) {
        close_list();
        note("%s", why);
        return -1;
    }

    char here[4096];
    snprintf(here, sizeof here, "%s", cwd ? cwd : "");

    static char          filter[4096];
    static struct anchor cur = {.col = -1};
    char                 notice[256] = {0};
    char                 ask[280] = {0};
    int                  asking = ASK_NONE;
    static int           been_here;

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
            note("no cards yet; /card <text> adds one");
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
            snprintf(title, sizeof title, "board · %s · %d card%s · %s%s%s",
                     where, n, n == 1 ? "" : "s", boardcfg_serving(), busy, left);
        else
            snprintf(title, sizeof title, "board · %s · %d of %d · %s%s%s", where,
                     shown, n, boardcfg_serving(), busy, left);

        char hint[512];
        if (notice[0])
            snprintf(hint, sizeof hint, "%s\n%s", notice, BOARD_HINT);
        else
            snprintf(hint, sizeof hint, "%s", BOARD_HINT);
        notice[0] = '\0';

        int pressed = 0;
        int cursor = -1;
        int at = vlist_run(title, &l, row_of(&l, &cur), hint, ask, BOARD_KEYS,
                           &pressed, board_tick, NULL, &cursor);
        int row = at >= 0 ? at : cursor;
        if (row >= 0)
            anchor_set(&cur, &l, row, at >= 0 ? -1 : (int)l.v[row].col);
        vlist_free(&l);

        if (at == PICK_REOPEN) {
            board_free(cards, n);
            continue;
        }
        if (asking) {
            int what = asking;
            asking = ASK_NONE;
            ask[0] = '\0';

            struct board_card *c = board_find(cards, n, cur.id);
            if (at >= 0 && pressed == 'y') {
                if (what == ASK_APPROVE_ALL) {
                    int did = approve_all(cards, n, filter);
                    snprintf(notice, sizeof notice, "approved %d card%s", did,
                             did == 1 ? "" : "s");
                } else if (c && do_delete(c))
                    cur.id[0] = '\0';
            }
            board_free(cards, n);
            if (at < 0) {
                close_list();
                return -1;
            }
            continue;
        }
        if (at < 0) {
            close_list();
            board_free(cards, n);
            return -1;
        }

        struct board_card *c = board_find(cards, n, cur.id);
        switch (pressed) {
        case 0:
            if (c) {
                close_list();
                card_form(c);
            }
            break;
        case KEY_NEW:
            close_list();
            do_new(filter[0] ? filter : here, cur.id);
            cur.col = -1;
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
        case KEY_START_MAX: {
            if (!in_backlog(cards, n, filter))
                snprintf(notice, sizeof notice, "nothing in backlog");
            else {
                char why[256];
                int  did = start_max(cards, n, filter, why, sizeof why);
                if (did)
                    snprintf(notice, sizeof notice, "started %d worker%s", did,
                             did == 1 ? "" : "s");
                else
                    snprintf(notice, sizeof notice, "%s",
                             why[0] ? why : "could not start a worker");
            }
            break;
        }
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
                    note("%s is running without a tab", step);
                else
                    note("no worker on that card");
            }
            break;
        }
        case KEY_APPROVE:
            approve(c, 0);
            break;
        case KEY_AUDIT:
            if (c && !boardsweep_is(c))
                approve(c, 1);
            break;
        case KEY_SKIP:
            if (c && c->col == BOARD_AUDIT) {
                if (!boardaudit_skippable())
                    snprintf(notice, sizeof notice, "the audit is not skippable");
                else {
                    boardwork_let_go(c->id);
                    boardaudit_skip(c);
                }
            }
            break;
        case KEY_APPROVE_ALL: {
            int ready = in_review(cards, n, filter);
            if (!ready)
                snprintf(notice, sizeof notice, "nothing in review");
            else {
                snprintf(ask, sizeof ask, "approve %d card%s in review?", ready,
                         ready == 1 ? "" : "s");
                asking = ASK_APPROVE_ALL;
            }
            break;
        }
        case KEY_UNDO:
            if (c && boardundo_can(c)) {
                char said[256];
                boardundo_run(c, said, sizeof said);
                snprintf(notice, sizeof notice, "%s", said);
            }
            break;
        case KEY_REJECT:
            if (c && boardsweep_is(c)) {
                boardwork_let_go(c->id);
                boardsweep_reject(c);
            } else if (c && (c->col == BOARD_REVIEW || c->col == BOARD_DOING ||
                             c->col == BOARD_DONE)) {
                close_list();
                char *why = ask_run(c->col == BOARD_DONE
                                        ? "why it is not fixed"
                                        : "reason for sending it back",
                                    NULL);
                if (why) {
                    boardwork_send_back(c, why);
                    free(why);
                }
            }
            break;
        case KEY_UNSTART:
            if (c)
                unstart(c);
            break;
        case KEY_FEEDBACK:
            if (c && boardwork_tab(c->id) >= 0) {
                close_list();
                char *say = ask_run("feedback for the worker", NULL);
                if (say) {
                    if (!boardwork_feedback(c, say))
                        note("could not send feedback to the worker");
                    free(say);
                }
            }
            break;
        case KEY_DELETE:
            if (c) {
                snprintf(ask, sizeof ask, "delete \"%s\"?", c->title);
                asking = ASK_DELETE;
            }
            break;
        case KEY_LOG: {
            char path[4300];
            if (c) {
                close_list();
                if (boardlog_path(c->id, path, sizeof path) && !edit_open(path))
                    note("no log for this card yet");
            }
            break;
        }
        case KEY_CONFIG: {
            close_list();
            boardcfgui_run();
            int waiting = 0;
            boardwork_serve(&waiting);
            break;
        }
        case KEY_SERVE:
            close_list();
            do_serve(notice, sizeof notice);
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
