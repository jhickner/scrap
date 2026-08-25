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
#include "boarddiff.h"
#include "boardcard.h"
#include "boardcfgui.h"
#include "boardflow.h"
#include "boardgrid.h"
#include "boardcmd.h"
#include "boardsweep.h"
#include "boardtile.h"
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
#define KEY_SWEEP    'w'
#define KEY_SERVE    'b'
#define KEY_ALL      '*'
#define KEY_VIEW     'v'

#define BOARD_KEYS "ndtsSgarxflcbw*Aiukv\t"

#define BOARD_RECENT_INDENT 6


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
        r->label = text_dsprintf("%s", text);
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

static int shows(const struct board_card *c, const char *filter)
{
    return !filter || !*filter || !strcmp(c->cwd, filter);
}

static int by_col(const void *a, const void *b)
{
    const struct board_card *const *x = a, *const *y = b;
    return board_cmp_col(*x, *y);
}

static int lane_cards(struct board_card *cards, int n, const char *filter,
                      int col, const char *const *walk, int walk_n,
                      const struct board_card **in, const char **name)
{
    enum board_col at = col < BOARD_STEP            ? (enum board_col)col
                        : col < BOARD_STEP + walk_n ? BOARD_STEP
                                      : (enum board_col)(col - walk_n + 1);
    const char *step = at == BOARD_STEP ? walk[col - BOARD_STEP] : NULL;

    int k = 0;
    for (int i = 0; i < n; i++) {
        if (cards[i].col != at || !shows(&cards[i], filter))
            continue;
        if (step && strcmp(cards[i].step, step))
            continue;
        in[k++] = &cards[i];
    }
    if (!k)
        return 0;
    qsort(in, (size_t)k, sizeof *in, by_col);

    int cap = at == BOARD_DONE      ? boardcfg()->done_shown
              : at == BOARD_BACKLOG ? boardcfg()->backlog_shown
                                    : 0;
    if (cap > 0 && k > cap)
        k = cap;

    *name = step ? step : board_col_name(at);
    return k;
}

static int build_board(struct vlist *l, struct board_card *cards, int n,
                       const char *filter, int wide)
{
    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    if (!in)
        return 0;
    int shown = 0;

    stamp_due = 0;

    const char *const *walk = NULL;
    int                walk_n = boardcfg_steps(&walk);

    for (int col = 0; col < BOARD_COLS - 1 + walk_n; col++) {
        const char *name = NULL;
        int k = lane_cards(cards, n, filter, col, walk, walk_n, in, &name);
        if (!k)
            continue;

        shown += k;
        row_heading(l, name);
        for (int i = 0; i < k; i++) {
            struct board_tile t;
            if (!boardtile_of(in[i], wide, &t))
                break;
            struct vrow *r = row_add(l);
            if (!r)
                break;

            snprintf(r->id, sizeof r->id, "%s", t.c->id);
            r->col = (unsigned char)t.c->col;
            r->label = text_dsprintf("%s", t.title);
            r->mark = t.mark;
            r->mark_role = t.mark_role;
            r->spin = t.spin;
            if (t.stamp)
                stamp_next(t.stamp);

            /* the kind and the pins are the tile's footer; the row runs
               them on the end of the status */
            char tail[128];
            snprintf(tail, sizeof tail, "%s%s%s", t.kind,
                     t.kind[0] && t.pins[0] ? " · " : "", t.pins);
            r->detail = text_dsprintf("%s%s%s", t.status,
                                      t.status[0] && tail[0] ? " · " : "", tail);

            for (int j = 0; j < t.recent_n; j++) {
                struct vrow *say = row_add(l);
                if (!say)
                    break;
                say->heading = PICK_TEXT;
                say->label = text_dsprintf("%*s%s", BOARD_RECENT_INDENT, "",
                                           t.recent[j]);
            }
        }
    }
    free(in);
    return shown;
}

struct glist {
    struct board_tile *t;
    int               *lane_of;
    const char       **name;
    int                n, lanes;
};

static void grid_free(struct glist *g)
{
    free(g->t);
    free(g->lane_of);
    free(g->name);
    memset(g, 0, sizeof *g);
}

static int build_grid(struct glist *g, struct board_card *cards, int n,
                      const char *filter, int wide)
{
    memset(g, 0, sizeof *g);

    const char *const *walk = NULL;
    int                walk_n = boardcfg_steps(&walk);
    int                lanes = BOARD_COLS - 1 + walk_n;

    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    g->t = calloc((size_t)(n ? n : 1), sizeof *g->t);
    g->lane_of = calloc((size_t)(n ? n : 1), sizeof *g->lane_of);
    g->name = calloc((size_t)lanes, sizeof *g->name);
    if (!in || !g->t || !g->lane_of || !g->name) {
        free(in);
        grid_free(g);
        return 0;
    }

    stamp_due = 0;

    for (int col = 0; col < lanes; col++) {
        const char *name = NULL;
        int k = lane_cards(cards, n, filter, col, walk, walk_n, in, &name);
        if (!k)
            continue;

        int lane = g->lanes++;
        g->name[lane] = name;
        for (int i = 0; i < k; i++) {
            struct board_tile *t = &g->t[g->n];
            if (!boardtile_of(in[i], wide, t))
                break;
            if (t->stamp)
                stamp_next(t->stamp);
            g->lane_of[g->n] = lane;
            g->n++;
        }
    }
    free(in);
    return g->n;
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
    int  lane;
};

static void anchor_set(struct anchor *a, const struct vlist *l, int row, int col)
{
    snprintf(a->id, sizeof a->id, "%s", l->v[row].id);
    a->col = col;
    a->lane = -1;
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

/* The card the anchor holds has left the row it was on: rest on what followed
   it, rather than following it to wherever it went. */
static void anchor_step(struct anchor *a)
{
    const char *to = a->next[0] ? a->next : a->prev;
    if (!*to)
        return;
    snprintf(a->id, sizeof a->id, "%s", to);
    a->next[0] = '\0';
    a->col = -1;
    a->lane = -1;
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

static void anchor_tile(struct anchor *a, const struct glist *g, int at, int col,
                        int lane)
{
    snprintf(a->id, sizeof a->id, "%s", g->t[at].c->id);
    a->col = col;
    a->lane = lane;
    a->next[0] = a->prev[0] = '\0';
    if (at + 1 < g->n)
        snprintf(a->next, sizeof a->next, "%s", g->t[at + 1].c->id);
    if (at > 0)
        snprintf(a->prev, sizeof a->prev, "%s", g->t[at - 1].c->id);
}

static int tile_at(const struct glist *g, const char *id)
{
    if (id && *id)
        for (int i = 0; i < g->n; i++)
            if (!strcmp(g->t[i].c->id, id))
                return i;
    return -1;
}

static int tile_of(const struct glist *g, const struct anchor *a)
{
    int at = tile_at(g, a->id);
    if (at >= 0 && (a->lane < 0 || g->lane_of[at] == a->lane))
        return at;

    int by = tile_at(g, a->next);
    if (by < 0)
        by = tile_at(g, a->prev);
    if (by >= 0)
        return by;
    return at >= 0 ? at : 0;
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
        changed |= boardcmd_take(key, out, ok);
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

    moved |= boardcmd_pump();
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
    const struct board_cfg *cfg = boardcfg();

    const char *backends[BOARD_BACKENDS_MAX + 1];
    int         backends_n =
        boardcfg_backend_choices(cfg, backends, COUNT(backends));
    const char *tiers[BOARD_TIERS + 1];
    int         tiers_n = boardcfg_tier_choices(tiers, COUNT(tiers));

    char spec[8192] = "";
    char backend[32] = "";
    char tier[8] = "";

    struct form_field fields[] = {
        {"spec", FORM_TEXT, spec, sizeof spec, NULL, 0, 0},
        {"backend", FORM_CHOICE, backend, sizeof backend, backends, backends_n, 0},
        {"tier", FORM_CHOICE, tier, sizeof tier, tiers, tiers_n, 0},
    };
    struct form f = {
        .title = "new card",
        .fields = fields,
        .fields_n = COUNT(fields),
    };
    if (!form_run(&f) || !spec[0])
        return;

    char id[BOARD_ID_MAX] = {0};
    if (!board_add(spec, cwd, id))
        return;
    snprintf(sel_id, BOARD_ID_MAX, "%s", id);
    if (backend[0] || tier[0])
        board_pin(id, backend[0] ? backend : NULL, tier[0] ? tier : NULL);
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

/* A kind with an approval prompt is not finished by approving it: the worker
 * is sent the prompt and the card ends on what it did. */
static char *approval_prompt_of(const struct board_card *c)
{
    if (!boardflow_waits_on_you(c))
        return NULL;
    return boardflow_approval(c);
}

static int approve(const struct board_card *c, int force)
{
    if (!boardflow_waits_on_you(c))
        return 0;
    if (boardsweep_is(c)) {
        boardwork_leave(c);
        boardwork_let_go(c->id);
        return boardsweep_approve(c);
    }
    char *say = approval_prompt_of(c);
    int   moved;
    if (!say)
        moved = boardwork_approve(c, force);
    else if (!(moved = boardwork_feedback(c, say)))
        note("no worker left to take it on");
    free(say);
    return moved;
}

static int in_review(const struct board_card *cards, int n, const char *filter)
{
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (boardflow_waits_on_you(&cards[i]) && shows(&cards[i], filter))
            ready++;
    return ready;
}

static int approve_all(const struct board_card *cards, int n, const char *filter)
{
    int did = 0;
    for (int i = 0; i < n; i++) {
        if (!boardflow_waits_on_you(&cards[i]) || !shows(&cards[i], filter))
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

static int unstart(const struct board_card *c)
{
    if (boardflow_runs(c) != BOARD_RUNS_WORKER || boardsweep_is(c))
        return 0;
    return boardwork_reject(c, "cancelled start");
}

static void save_card(const char *id, const struct boardcard_edit *e)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *live = board_find(cards, n, id);
    if (!live) {
        board_free(cards, n);
        return;
    }

    struct board_card edited = *live;
    snprintf(edited.kind, sizeof edited.kind, "%s", e->kind);
    snprintf(edited.backend_pin, sizeof edited.backend_pin, "%s", e->backend);
    snprintf(edited.tier_pin, sizeof edited.tier_pin, "%s", e->tier);
    board_put(&edited, e->column);
    edited.priority = atoi(e->priority);

    int respec = !e->proposals &&
                 strcmp(e->spec, live->body ? live->body : "") != 0;
    if (!e->proposals) {
        edited.body = (char *)e->spec;
        if (!live->kind[0])
            board_title_of(e->spec, edited.title, sizeof edited.title);
    }

    char *full = path_expand_home(e->where);
    if (full && *full)
        snprintf(edited.cwd, sizeof edited.cwd, "%s", full);
    free(full);

    int answered = respec && live->col == BOARD_UNCLEAR &&
                   edited.col == BOARD_UNCLEAR;
    if (answered)
        edited.col = BOARD_NEW;

    int repin = strcmp(edited.backend_pin, live->backend_pin) != 0 ||
                strcmp(edited.tier_pin, live->tier_pin) != 0;
    int ok = board_update(&edited);
    board_free(cards, n);

    if (ok && repin) {
        int waiting = 0;
        boardwork_serve(&waiting);
    }

    if (ok && answered) {
        board_note(id, "you", "spec edited; re-triaging");
        struct board_card *again = NULL;
        int                m = board_load(&again);
        struct board_card *fresh = board_find(again, m, id);
        if (fresh)
            boardtriage_start(fresh);
        board_free(again, m);
    }
}

static void do_card(const struct board_card *c)
{
    struct boardcard_edit edit;
    enum boardcard_action act = boardcard_form(c, &edit);
    if (act == BOARDCARD_NONE)
        return;
    if (act == BOARDCARD_SAVE) {
        save_card(c->id, &edit);
        return;
    }

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *live = board_find(cards, n, c->id);
    if (live) {
        if (act == BOARDCARD_UNSTART)
            unstart(live);
        else
            approve(live, 0);
    }
    board_free(cards, n);
}

int boardview_approve(const char *id, char *why, int size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    int ok = 0;
    if (!c)
        snprintf(why, (size_t)size, "card %s is not on the board", id);
    else if (!boardflow_waits_on_you(c))
        snprintf(why, (size_t)size, "card %s is in %s, and can't be approved "
                 "yet", id, board_where(c));
    else {
        approve(c, 0);
        ok = 1;
    }

    board_free(cards, n);
    return ok;
}

static void steps_of(const char *kind, char *out, size_t size)
{
    size_t used = 0;
    out[0] = '\0';
    for (int at = 0;; at++) {
        const char *step = boardcfg_kind_step(kind, at);
        if (!step)
            return;
        used += (size_t)snprintf(out + used, size - used, "%s%s",
                                 used ? ", " : "", step);
        if (used >= size)
            return;
    }
}

int boardview_moveto(const char *id, const char *step, char *why, int size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    char steps[512];
    int  ok = 0;
    if (!c)
        snprintf(why, (size_t)size, "card %s is not on the board", id);
    else if (!boardcfg_kind_takes(c->kind, step)) {
        steps_of(c->kind, steps, sizeof steps);
        snprintf(why, (size_t)size, "%s has no %s step%s%s",
                 c->kind[0] ? c->kind : "this card", step,
                 steps[0] ? " \xc2\xb7 " : "", steps);
    } else if (board_at(c, step))
        snprintf(why, (size_t)size, "card %s is already in %s", id, step);
    else {
        boardwork_leave(c);
        boardwork_let_go(c->id);
        ok = board_move_to(c->id, step, "you", NULL);
        if (!ok)
            snprintf(why, (size_t)size, "could not move card %s", id);
    }

    board_free(cards, n);
    return ok;
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
        return BOARDVIEW_NONE;
    }

    char here[4096];
    snprintf(here, sizeof here, "%s", cwd ? cwd : "");

    static char          filter[4096];
    static struct anchor cur = {.col = -1, .lane = -1};
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

        int grid = !strcmp(boardcfg_view(), "grid");

        struct vlist l = {0};
        struct glist g = {0};
        int          shown = grid ? build_grid(&g, cards, n, filter, !filter[0])
                                  : build_board(&l, cards, n, filter, !filter[0]);

        if (!(grid ? g.n : l.n)) {
            vlist_free(&l);
            grid_free(&g);
            board_free(cards, n);

            if (filter[0]) {
                filter[0] = '\0';
                continue;
            }
            close_list();
            note("no cards yet; /card <text> to create");
            return BOARDVIEW_NONE;
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
        snprintf(hint, sizeof hint, "%s", notice);
        notice[0] = '\0';

        int pressed = 0;
        int cursor = -1;
        int part = GRID_PART_TILE;
        int at;

        if (grid) {
            at = boardgrid_run(title, g.t, g.lane_of, g.name, g.n, g.lanes,
                               tile_of(&g, &cur), hint, ask, BOARD_KEYS,
                               &pressed, board_tick, NULL, &cursor, &part);
            if (at == GRID_NARROW) {
                grid_free(&g);
                grid = 0;
                size_t used = strlen(hint);
                snprintf(hint + used, sizeof hint - used, "  ·  / search");
                build_board(&l, cards, n, filter, !filter[0]);
            }
        }
        if (!grid)
            at = vlist_run(title, &l, row_of(&l, &cur), hint, ask, BOARD_KEYS,
                           &pressed, board_tick, NULL, &cursor);

        int row = at >= 0 ? at : cursor;
        if (row >= 0) {
            if (grid)
                anchor_tile(&cur, &g, row, at >= 0 ? -1 : (int)g.t[row].c->col,
                            at >= 0 ? -1 : g.lane_of[row]);
            else
                anchor_set(&cur, &l, row, at >= 0 ? -1 : (int)l.v[row].col);
        }
        if (grid && at >= 0 && !pressed && part == GRID_PART_STATUS)
            pressed = KEY_GO;
        vlist_free(&l);
        grid_free(&g);

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
        if (pressed == '\t') {
            close_list();
            board_free(cards, n);
            return BOARDVIEW_SESSIONS;
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
                do_card(c);
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
            const char *step = c ? boardtile_step(c->id) : NULL;
            char        why[256] = "";
            if (tab < 0 && c && !step)
                tab = boardwork_rejoin(c, why, sizeof why);
            if (tab >= 0) {
                close_list();
                board_free(cards, n);
                return tab;
            }
            if (c) {
                close_list();
                if (step)
                    note("%s is running without a tab", step);
                else
                    note("%s", why[0] ? why : "no worker on that card");
            }
            break;
        }
        case KEY_APPROVE:
            if (approve(c, 0))
                anchor_step(&cur);
            break;
        case KEY_AUDIT:
            if (c && !boardsweep_is(c) && approve(c, 1))
                anchor_step(&cur);
            break;
        case KEY_SKIP:
            if (c) {
                if (c->col != BOARD_STEP)
                    snprintf(notice, sizeof notice, "no step to skip here");
                else if (!boardflow_skippable(c))
                    snprintf(notice, sizeof notice, "%s is not skippable",
                             c->step);
                else {
                    boardwork_halt(c->id);
                    boardflow_skip(c);
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
            } else if (c && (c->col == BOARD_STEP || c->col == BOARD_DONE)) {
                close_list();
                const struct board_role *at = boardflow_role(c);
                char                     asked[64];
                if (c->col == BOARD_DONE)
                    snprintf(asked, sizeof asked, "why it is not fixed");
                else
                    snprintf(asked, sizeof asked, "reason to %s",
                             at ? at->fail_label : "send it back");
                char *why = ask_run(asked, NULL);
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
        case KEY_SWEEP: {
            const char *cwd = c && c->cwd[0] ? c->cwd : filter[0] ? filter : here;
            char        why[256];
            if (boardwork_sweep_now(cwd, why, sizeof why))
                snprintf(notice, sizeof notice, "sweep started");
            else
                snprintf(notice, sizeof notice, "%s", why);
            break;
        }
        case KEY_SERVE:
            close_list();
            do_serve(notice, sizeof notice);
            break;
        case KEY_VIEW:
            boardcfg_set_view(strcmp(boardcfg_view(), "grid") ? "grid" : "list");
            cur.lane = -1;
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
