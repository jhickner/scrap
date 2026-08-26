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
#include "boardtile.h"
#include "boardname.h"
#include "boardwork.h"
#include "child.h"
#include "chrome.h"
#include "form.h"
#include "gitcmd.h"
#include "pick.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"
#include "session.h"
#include "workspace.h"

#define KEY_NEW      'n'
#define KEY_DELETE   'd'
#define KEY_START    's'
#define KEY_START_MAX 'S'
#define KEY_GO       'g'
#define KEY_CLOSE    'a'
#define KEY_CLOSE_ALL 'A'
#define KEY_STOP     'r'
#define KEY_RUN      'R'
#define KEY_UNSTART  'x'
#define KEY_FEEDBACK 'f'
#define KEY_LOG      'l'
#define KEY_CONFIG   'c'
#define KEY_SERVE    'b'
#define KEY_ALL      '*'
#define KEY_VIEW     'v'

#define BOARD_KEYS "ndsSgarRxflcb*Av\t"

#define BOARD_RECENT_INDENT 6


struct vrow {
    char          id[BOARD_ID_MAX];
    unsigned char stand;
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

static int by_order(const void *a, const void *b)
{
    const struct board_card *const *x = a, *const *y = b;
    return board_cmp(*x, *y);
}

/* A lane is a stand, except that the working stand runs one lane per action,
   so a card being worked stands under the step it is on. */
struct lane {
    enum board_stand stand;
    const char      *step;
};

static int lane_holds(const struct board_card *c, const struct lane *lane)
{
    if (board_stands(c) != lane->stand)
        return 0;
    if (!lane->step)
        return 1;
    const char *step = board_step(c);
    return step && !strcmp(step, lane->step);
}

static int laned(const struct lane *lanes, int n, const char *step)
{
    for (int i = 0; i < n; i++)
        if (lanes[i].step && !strcmp(lanes[i].step, step))
            return 1;
    return 0;
}

/* The steps in the order the config names them, then anything a card is on
   that the config no longer has. */
static int work_lanes(const struct board_card *cards, int n, const char *filter,
                      struct lane *out, int max)
{
    const char *acts[BOARD_ACTIONS_MAX];
    int         acts_n = boardcfg_actions(acts, BOARD_ACTIONS_MAX);
    int         k = 0;

    for (int i = 0; i < acts_n && k < max; i++)
        for (int j = 0; j < n; j++) {
            const char *step = board_step(&cards[j]);
            if (board_stands(&cards[j]) != BOARD_WORKING ||
                !shows(&cards[j], filter) || !step || strcmp(step, acts[i]))
                continue;
            out[k].stand = BOARD_WORKING;
            out[k].step = acts[i];
            k++;
            break;
        }

    for (int j = 0; j < n && k < max; j++) {
        const char *step = board_step(&cards[j]);
        if (board_stands(&cards[j]) != BOARD_WORKING ||
            !shows(&cards[j], filter) || !step || laned(out, k, step))
            continue;
        out[k].stand = BOARD_WORKING;
        out[k].step = step;
        k++;
    }
    return k;
}

static int board_lanes(const struct board_card *cards, int n, const char *filter,
                       struct lane *out, int max)
{
    int k = 0;
    if (k < max)
        out[k++] = (struct lane){BOARD_OPEN, NULL};
    k += work_lanes(cards, n, filter, out + k, max - k);
    if (k < max)
        out[k++] = (struct lane){BOARD_REVIEW, NULL};
    if (k < max)
        out[k++] = (struct lane){BOARD_CLOSED, NULL};
    return k;
}

static int lane_cards(struct board_card *cards, int n, const char *filter,
                      const struct lane *lane, const struct board_card **in,
                      const char **name)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        if (lane_holds(&cards[i], lane) && shows(&cards[i], filter))
            in[k++] = &cards[i];
    if (!k)
        return 0;
    qsort(in, (size_t)k, sizeof *in, by_order);

    int cap = lane->stand == BOARD_CLOSED ? boardcfg()->closed_shown
              : lane->stand == BOARD_OPEN ? boardcfg()->open_shown
                                          : 0;
    if (cap > 0 && k > cap)
        k = cap;

    *name = lane->step ? lane->step : board_stand_name(lane->stand);
    return k;
}

static int build_board(struct vlist *l, struct board_card *cards, int n,
                       const char *filter, int wide)
{
    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    struct lane *lanes = calloc((size_t)(n + BOARD_STANDS), sizeof *lanes);
    if (!in || !lanes) {
        free(in);
        free(lanes);
        return 0;
    }
    int shown = 0;

    stamp_due = 0;

    int lanes_n = board_lanes(cards, n, filter, lanes, n + BOARD_STANDS);
    for (int lane = 0; lane < lanes_n; lane++) {
        const char *name = NULL;
        int k = lane_cards(cards, n, filter, &lanes[lane], in, &name);
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
            r->stand = (unsigned char)board_stands(t.c);
            r->label = text_dsprintf("%s", t.title);
            r->mark = t.mark;
            r->mark_role = t.mark_role;
            r->spin = t.spin;
            if (t.stamp)
                stamp_next(t.stamp);

            /* the pins are the tile's footer; the row runs them on the end
               of the status */
            r->detail = text_dsprintf("%s%s%s", t.status,
                                      t.status[0] && t.pins[0] ? " · " : "",
                                      t.pins);

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
    free(lanes);
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

    int max = n + BOARD_STANDS;

    const struct board_card **in = calloc((size_t)(n ? n : 1), sizeof *in);
    struct lane *lanes = calloc((size_t)max, sizeof *lanes);
    g->t = calloc((size_t)(n ? n : 1), sizeof *g->t);
    g->lane_of = calloc((size_t)(n ? n : 1), sizeof *g->lane_of);
    g->name = calloc((size_t)max, sizeof *g->name);
    if (!in || !lanes || !g->t || !g->lane_of || !g->name) {
        free(in);
        free(lanes);
        grid_free(g);
        return 0;
    }

    stamp_due = 0;

    int lanes_n = board_lanes(cards, n, filter, lanes, max);
    for (int at = 0; at < lanes_n; at++) {
        const char *name = NULL;
        int k = lane_cards(cards, n, filter, &lanes[at], in, &name);
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
    free(lanes);
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

/* stand is where the card stood when the cursor last rested on it, or -1 to
   follow that card wherever it has gone. */
struct anchor {
    char id[BOARD_ID_MAX];
    char next[BOARD_ID_MAX];
    char prev[BOARD_ID_MAX];
    int  stand;
    int  lane;
};

static void anchor_set(struct anchor *a, const struct vlist *l, int row,
                       int stand)
{
    snprintf(a->id, sizeof a->id, "%s", l->v[row].id);
    a->stand = stand;
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
    a->stand = -1;
    a->lane = -1;
}

static int row_of(const struct vlist *l, const struct anchor *a)
{
    int at = find_id(l, a->id);
    if (at >= 0 && (a->stand < 0 || (int)l->v[at].stand == a->stand))
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

static void anchor_tile(struct anchor *a, const struct glist *g, int at,
                        int stand, int lane)
{
    snprintf(a->id, sizeof a->id, "%s", g->t[at].c->id);
    a->stand = stand;
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
        (void)ok;
        changed |= boardname_take(key, out);
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
    if (!boardview_capture(spec, cwd, id, sizeof id))
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

enum { ASK_NONE, ASK_CLOSE, ASK_CLOSE_ALL, ASK_DELETE };

static int do_delete(const struct board_card *c)
{
    boardwork_discard(c);
    return board_remove(c->id);
}

static int run_action(const struct board_card *c, const char *name)
{
    char why[256];
    if (boardflow_trigger(c, &name, 1, why, sizeof why))
        return 1;
    note("%s", why);
    return 0;
}

/* Put an action on the card, then put a worker on it. Both halves want the
   card as it stands on disk, so it is loaded again between them. */
static int run_now(const char *id, const char *name, char *why, int size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    int queued = c && boardflow_trigger(c, &name, 1, why, (size_t)size);
    board_free(cards, n);
    if (!queued)
        return 0;

    if (boardwork_tab(id) >= 0) {
        boardwork_step(id);
        return 1;
    }

    n = board_load(&cards);
    c = board_find(cards, n, id);

    /* the first action on a card checks its worktree out, which git takes long
       enough over that the board would otherwise stand blank for it */
    if (c && !c->worktree[0] && boardflow_worktree(c))
        note("making a worktree for card %s", c->id);

    int ok = c && boardwork_start(c, why, size);
    board_free(cards, n);
    return ok;
}

static int in_review(const struct board_card *cards, int n, const char *filter)
{
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (boardflow_waits_on_you(&cards[i]) && shows(&cards[i], filter))
            ready++;
    return ready;
}

static int unmerged(const struct board_card *cards, int n, const char *filter)
{
    int loses = 0;
    for (int i = 0; i < n; i++)
        if (boardflow_waits_on_you(&cards[i]) && shows(&cards[i], filter) &&
            boardwork_unmerged(&cards[i]))
            loses++;
    return loses;
}

static int close_card(const char *id)
{
    if (!board_close(id))
        return 0;
    boardwork_let_go(id);
    return 1;
}

static int close_all(const struct board_card *cards, int n, const char *filter)
{
    int did = 0;
    for (int i = 0; i < n; i++) {
        if (!boardflow_waits_on_you(&cards[i]) || !shows(&cards[i], filter))
            continue;
        close_card(cards[i].id);
        did++;
    }
    return did;
}

/* a card with a queue and no session on it is one a worker can be put on */
static int waiting(const struct board_card *c, const char *filter)
{
    return board_stands(c) == BOARD_WORKING && boardwork_tab(c->id) < 0 &&
           shows(c, filter);
}

static int can_start(const struct board_card *cards, int n, const char *filter)
{
    int ready = 0;
    for (int i = 0; i < n; i++)
        if (waiting(&cards[i], filter))
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
        if (waiting(&cards[i], filter))
            in[k++] = &cards[i];
    qsort(in, (size_t)k, sizeof *in, by_order);

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
    if (!c || board_stands(c) != BOARD_WORKING)
        return 0;
    boardwork_let_go(c->id);
    return board_stopped(c->id);
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
    snprintf(edited.backend_pin, sizeof edited.backend_pin, "%s", e->backend);
    snprintf(edited.tier_pin, sizeof edited.tier_pin, "%s", e->tier);
    edited.priority = atoi(e->priority);
    edited.body = (char *)e->spec;

    /* a title nobody has named still follows the spec it was taken from */
    char taken[BOARD_TITLE_MAX];
    board_title_of(live->body ? live->body : "", taken, sizeof taken);
    if (!strcmp(live->title, taken))
        board_title_of(e->spec, edited.title, sizeof edited.title);

    char *full = path_expand_home(e->where);
    if (full && *full)
        snprintf(edited.cwd, sizeof edited.cwd, "%s", full);
    free(full);

    int repin = strcmp(edited.backend_pin, live->backend_pin) != 0 ||
                strcmp(edited.tier_pin, live->tier_pin) != 0;
    int ok = board_update(&edited);
    board_free(cards, n);

    if (ok && repin) {
        int waiting = 0;
        boardwork_serve(&waiting);
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
        else if (act == BOARDCARD_RUN)
            run_action(live, edit.run);
        else if (act == BOARDCARD_CLOSE)
            close_card(live->id);
    }
    board_free(cards, n);
}

int boardview_close(const char *id, char *why, int size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    int ok = 0;
    if (!c)
        snprintf(why, (size_t)size, "card %s is not on the board", id);
    else if (!boardflow_waits_on_you(c))
        snprintf(why, (size_t)size, "card %s is %s, and can't be closed yet",
                 id, board_stand_name(board_stands(c)));
    else {
        int left = boardwork_unmerged(c);
        close_card(c->id);
        if (left)
            snprintf(why, (size_t)size,
                     "%d commit%s %s not merged, and the branch goes with the "
                     "card", left, left == 1 ? "" : "s",
                     left == 1 ? "was" : "were");
        ok = 1;
    }

    board_free(cards, n);
    return ok;
}

/* "implement, merge" -> the actions it names, in order. Eats its argument. */
static int names_of(char *spec, const char **out, int max)
{
    int n = 0;
    for (char *at = spec; *at && n < max;) {
        while (*at == ' ' || *at == ',')
            at++;
        char *start = at;
        while (*at && *at != ',')
            at++;
        char *end = at;
        if (*at)
            at++;
        while (end > start && end[-1] == ' ')
            end--;
        *end = '\0';
        if (*start)
            out[n++] = start;
    }
    return n;
}

static void offered_of(const struct board_card *c, char *out, size_t size)
{
    const char *offered[BOARD_ACTIONS_MAX];
    int         n = boardflow_offered(c, offered, BOARD_ACTIONS_MAX);
    if (!n) {
        snprintf(out, size, "nothing can run on this card yet");
        return;
    }

    size_t at = (size_t)snprintf(out, size, "run one of:");
    for (int i = 0; i < n && at < size; i++)
        at += (size_t)snprintf(out + at, size - at, "%s %s", i ? "," : "",
                               offered[i]);
}

int boardview_trigger(const char *id, const char *spec, char *why, int size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    int ok = 0;
    if (!c) {
        snprintf(why, (size_t)size, "card %s is not on the board", id);
    } else if (!spec || !*spec) {
        offered_of(c, why, (size_t)size);
    } else {
        char        copy[256];
        const char *names[BOARD_QUEUE];
        snprintf(copy, sizeof copy, "%s", spec);
        int k = names_of(copy, names, BOARD_QUEUE);
        ok = boardflow_trigger(c, names, k, why, (size_t)size);
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

    /* the card is open the moment it is captured, carrying its own first line;
       naming runs alongside and improves the title if it comes back */
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    boardname_start(board_find(cards, n, id));
    board_free(cards, n);
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
    if (!cwd || !*cwd || !gitcmd_root(cwd, here, sizeof here))
        snprintf(here, sizeof here, "%s", cwd ? cwd : "");

    static char          filter[4096];
    static struct anchor cur = {.stand = -1, .lane = -1};
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
                anchor_tile(&cur, &g, row,
                            at >= 0 ? -1 : (int)board_stands(g.t[row].c),
                            at >= 0 ? -1 : g.lane_of[row]);
            else
                anchor_set(&cur, &l, row, at >= 0 ? -1 : (int)l.v[row].stand);
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
                if (what == ASK_CLOSE) {
                    if (c && close_card(c->id))
                        anchor_step(&cur);
                } else if (what == ASK_CLOSE_ALL) {
                    int did = close_all(cards, n, filter);
                    snprintf(notice, sizeof notice, "closed %d card%s", did,
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
            cur.stand = -1;
            break;
        case KEY_START:
            if (c) {
                char why[256];
                if (!boardwork_start(c, why, sizeof why))
                    snprintf(notice, sizeof notice, "%s", why);
            }
            break;
        case KEY_START_MAX: {
            if (!can_start(cards, n, filter))
                snprintf(notice, sizeof notice, "nothing queued to start");
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
        case KEY_CLOSE: {
            if (!c)
                break;
            int left = boardwork_unmerged(c);
            if (left) {
                snprintf(ask, sizeof ask,
                         "%d commit%s on this card %s not merged, and close "
                         "deletes the branch; close it?", left,
                         left == 1 ? "" : "s", left == 1 ? "is" : "are");
                asking = ASK_CLOSE;
                break;
            }
            if (close_card(c->id))
                anchor_step(&cur);
            break;
        }
        case KEY_CLOSE_ALL: {
            int ready = in_review(cards, n, filter);
            if (!ready)
                snprintf(notice, sizeof notice, "nothing in review");
            else {
                int loses = unmerged(cards, n, filter);
                if (loses)
                    snprintf(ask, sizeof ask,
                             "close %d card%s in review? %d still hold%s an "
                             "unmerged branch", ready, ready == 1 ? "" : "s",
                             loses, loses == 1 ? "s" : "");
                else
                    snprintf(ask, sizeof ask, "close %d card%s in review?",
                             ready, ready == 1 ? "" : "s");
                asking = ASK_CLOSE_ALL;
            }
            break;
        }
        case KEY_RUN: {
            if (!c)
                break;
            const char *offered[BOARD_ACTIONS_MAX];
            int         on = boardflow_offered(c, offered, BOARD_ACTIONS_MAX);
            if (!on) {
                snprintf(notice, sizeof notice,
                         "nothing can run on this card yet");
                break;
            }

            struct pick_item items[BOARD_ACTIONS_MAX];
            for (int i = 0; i < on; i++)
                items[i] = (struct pick_item){offered[i], NULL};

            char id[BOARD_ID_MAX];
            snprintf(id, sizeof id, "%s", c->id);
            close_list();
            int chosen = pick_run("run on this card", items, on, 0);
            if (chosen < 0)
                break;

            char why[256] = "";
            char name[64];
            snprintf(name, sizeof name, "%s", offered[chosen]);
            if (run_now(id, name, why, sizeof why))
                snprintf(notice, sizeof notice, "running %s", name);
            else
                snprintf(notice, sizeof notice, "%s",
                         why[0] ? why : "could not run it");
            break;
        }
        case KEY_STOP:
            if (c && board_stands(c) != BOARD_OPEN) {
                close_list();
                char *why = ask_run("what is wrong with it", NULL);
                if (why) {
                    boardwork_stop(c, why);
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
