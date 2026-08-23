#include "boardview.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ask.h"
#include "board.h"
#include "confirm.h"
#include "pick.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"

// The list holds letters for itself, so searching is behind '/' and a typed
// letter means the key it stands for.
#define KEY_NEW    'n'
#define KEY_MOVE   'm'
#define KEY_EDIT   'e'
#define KEY_REPO   'w'
#define KEY_DELETE 'd'
#define KEY_ALL    '*'

#define BOARD_KEYS "nmewd*"

// How wide a card's title may grow before the meta beside it stops lining up.
#define TITLE_SHARE(cols) ((cols) * 3 / 5)

/* ---- rows ------------------------------------------------------------- */

struct vrow {
    char          id[BOARD_ID_MAX];  /* empty for anything not a card */
    char         *label;
    char         *detail;
    unsigned char heading;
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

// Indented under the heading it sits below, to the depth pick draws its own
// rows at, so content and choices line up as one column.
static void row_text(struct vlist *l, const char *text)
{
    struct vrow *r = row_add(l);
    if (r) {
        r->label = dsprintf("  %s", text);
        r->heading = PICK_TEXT;
    }
}

// A row the highlight passes over, which is not the same as a row set apart:
// PICK_APART rows are ordinary choices that merely want a gap above them.
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

// Runs a built list through the picker, which wants its columns as separate
// arrays. Returns the row that was chosen, or -1.
static int vlist_run(const char *title, struct vlist *l, int initial,
                     const char *shortcuts, int *pressed)
{
    struct pick_item *items = calloc((size_t)l->n, sizeof *items);
    unsigned char    *heading = calloc((size_t)l->n, 1);
    const char      **mark = calloc((size_t)l->n, sizeof *mark);
    unsigned char    *role = calloc((size_t)l->n, 1);
    if (!items || !heading || !mark || !role) {
        free(items);
        free(heading);
        free(mark);
        free(role);
        return -1;
    }

    for (int i = 0; i < l->n; i++) {
        items[i].label = l->v[i].label ? l->v[i].label : "";
        items[i].detail = l->v[i].detail;
        heading[i] = l->v[i].heading;
        mark[i] = l->v[i].mark;
        role[i] = l->v[i].mark_role;
    }

    struct pick_live live = {
        .heading = heading,
        .mark = mark,
        .mark_role = role,
    };
    int at = pick_run_live(title, items, l->n, initial, &live, PICK_SEARCH_SLASH,
                           shortcuts, pressed);

    free(items);
    free(heading);
    free(mark);
    free(role);
    return at;
}

/* ---- what a card looks like ------------------------------------------- */

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

// The status column: what the card is doing, ahead of what it is.
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

// A repo, short enough to sit beside a title. What identifies a worktree is
// its tail, so that is the end that is kept.
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

static int shows(const struct board_card *c, const char *filter)
{
    return !filter || !*filter || !strcmp(c->cwd, filter);
}

// Every card in a column, in the order the board reads them: by priority, then
// by how long they have been waiting.
static int by_priority(const void *a, const void *b)
{
    const struct board_card *const *x = a, *const *y = b;
    if ((*x)->priority != (*y)->priority)
        return (*y)->priority - (*x)->priority;
    if ((*x)->created != (*y)->created)
        return (*x)->created < (*y)->created ? -1 : 1;
    return strcmp((*x)->id, (*y)->id);
}

// Returns how many cards the filter let through, which is not `n` and is what
// the title bar has to say.
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

            char when[32];
            ago(c->updated ? c->updated : c->created, when, sizeof when);

            // In one repo the directory is the title bar's job; across repos
            // it is the first thing you need from a row.
            char where[256] = {0};
            if (wide)
                short_repo(c->cwd, where, sizeof where);

            if (where[0] && c->kind[0])
                r->detail = dsprintf("%s · %s · %s", where, c->kind, when);
            else if (where[0])
                r->detail = dsprintf("%s · %s", where, when);
            else if (c->kind[0])
                r->detail = dsprintf("%s · %s", c->kind, when);
            else
                r->detail = dsprintf("%s", when);
        }
    }
    free(in);
    return shown;
}

// Pads every card's title to one width, so the meta beside them reads as a
// column rather than as ragged tails. Headings are left where they are.
static void align(struct vlist *l)
{
    size_t width = 0;
    size_t cap = (size_t)TITLE_SHARE(ui_columns());

    for (int i = 0; i < l->n; i++) {
        if (is_text(&l->v[i]) || !l->v[i].detail)
            continue;
        size_t cells = ui_cells(l->v[i].label);
        if (cells > width)
            width = cells;
    }
    if (width > cap)
        width = cap;
    if (!width)
        return;

    for (int i = 0; i < l->n; i++) {
        if (is_text(&l->v[i]) || !l->v[i].detail)
            continue;
        size_t cells = ui_cells(l->v[i].label);
        if (cells >= width)
            continue;
        char *padded = malloc(strlen(l->v[i].label) + (width - cells) + 1);
        if (!padded)
            continue;
        strcpy(padded, l->v[i].label);
        memset(padded + strlen(l->v[i].label), ' ', width - cells);
        padded[strlen(l->v[i].label) + (width - cells)] = '\0';
        free(l->v[i].label);
        l->v[i].label = padded;
    }
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

/* ---- the things the keys do ------------------------------------------- */

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

static void do_move(const struct board_card *c)
{
    struct pick_item cols[BOARD_COLS];
    for (int i = 0; i < BOARD_COLS; i++) {
        cols[i].label = board_col_name((enum board_col)i);
        cols[i].detail = NULL;
    }
    int at = pick_run("move to", cols, BOARD_COLS, (int)c->col);
    if (at >= 0 && at != (int)c->col)
        board_move(c->id, (enum board_col)at, "you", NULL);
}

static void do_edit(const struct board_card *c)
{
    char *title = ask_run("title", c->title);
    if (!title)
        return;
    if (*title) {
        struct board_card edited = *c;
        snprintf(edited.title, sizeof edited.title, "%s", title);
        board_update(&edited);
    }
    free(title);
}

static void do_repo(const struct board_card *c)
{
    char *path = ask_run("repo", c->cwd);
    if (!path)
        return;
    char *full = path_expand_home(path);
    if (full && *full) {
        struct board_card edited = *c;
        snprintf(edited.cwd, sizeof edited.cwd, "%s", full);
        board_update(&edited);
    }
    free(full);
    free(path);
}

static int do_delete(const struct board_card *c)
{
    char question[280];
    snprintf(question, sizeof question, "delete \"%s\"?", c->title);
    if (!confirm_run(question))
        return 0;
    return board_remove(c->id);
}

/* ---- the detail screen ------------------------------------------------ */

enum {
    ACT_NONE,
    ACT_MOVE,
    ACT_EDIT,
    ACT_REPO,
    ACT_DELETE,
};

static void row_action(struct vlist *l, int action, const char *label, const char *key)
{
    struct vrow *r = row_add(l);
    if (!r)
        return;
    r->label = dsprintf("%s", label);
    r->detail = dsprintf("%s", key);
    r->action = action;
    // The first of them takes the blank line that sets the actions off from
    // whatever the card had to say.
    if (l->n == 1 || l->v[l->n - 2].action == ACT_NONE)
        r->heading = PICK_APART;
}

// A paragraph as rows, so it reads as one block rather than a truncated line.
static void row_paragraph(struct vlist *l, const char *text)
{
    size_t budget = (size_t)ui_columns() - 8;
    if ((int)budget < 8)
        budget = 8;

    const char *p = text;
    size_t      left = strlen(text);
    int         rows = 0;

    while (left && rows < 40) {
        size_t skip = 0;
        size_t take = ui_wrap_row(p, left, budget, &skip, NULL);
        if (!take && !skip)
            break;

        char line[1024];
        size_t n = take < sizeof line - 1 ? take : sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        row_text(l, line);
        rows++;

        p += take + skip;
        left -= take + skip;
    }
}

static void build_detail(struct vlist *l, const struct board_card *c)
{
    if (c->body && *c->body && strcmp(c->body, c->title)) {
        row_heading(l, "spec");
        row_paragraph(l, c->body);
    }

    row_heading(l, "where");
    char where[512];
    path_home_relative(c->cwd, where, sizeof where);
    row_text(l, where[0] ? where : "(nowhere yet)");

    if (c->log_n) {
        row_heading(l, "log");
        for (int i = 0; i < c->log_n; i++) {
            struct tm when;
            char      stamp[16] = "     ";
            if (c->log[i].ts) {
                localtime_r(&c->log[i].ts, &when);
                strftime(stamp, sizeof stamp, "%H:%M", &when);
            }
            char line[1024];
            snprintf(line, sizeof line, "%s  %-6s %s", stamp, c->log[i].who,
                     c->log[i].text ? c->log[i].text : "");
            row_text(l, line);
        }
    }

    row_action(l, ACT_MOVE, "move to another column", "m");
    row_action(l, ACT_EDIT, "edit the title", "e");
    row_action(l, ACT_REPO, "set the repo", "w");
    row_action(l, ACT_DELETE, "delete", "d");
}

// Returns zero when the card is gone, so the board does not try to put the
// highlight back on it.
static int detail_run(const char *id)
{
    for (;;) {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, id);
        if (!c) {
            board_free(cards, n);
            return 0;
        }

        struct vlist l = {0};
        build_detail(&l, c);
        align(&l);

        char title[600];
        snprintf(title, sizeof title, "%s · %s · %s", c->id,
                 c->kind[0] ? c->kind : "unsorted", board_col_name(c->col));

        int pressed = 0;
        int at = vlist_run(title, &l, row_of(&l, NULL), BOARD_KEYS, &pressed);

        int action = ACT_NONE;
        if (at < 0) {
            vlist_free(&l);
            board_free(cards, n);
            return 1;
        }
        switch (pressed) {
        case KEY_MOVE:   action = ACT_MOVE; break;
        case KEY_EDIT:   action = ACT_EDIT; break;
        case KEY_REPO:   action = ACT_REPO; break;
        case KEY_DELETE: action = ACT_DELETE; break;
        case 0:          action = l.v[at].action; break;
        default:         break;
        }
        vlist_free(&l);

        int gone = 0;
        switch (action) {
        case ACT_MOVE:   do_move(c); break;
        case ACT_EDIT:   do_edit(c); break;
        case ACT_REPO:   do_repo(c); break;
        case ACT_DELETE: gone = do_delete(c); break;
        default:         break;
        }
        board_free(cards, n);
        if (gone)
            return 0;
    }
}

/* ---- the board -------------------------------------------------------- */

int boardview_capture(const char *text, const char *cwd, char *id_out, int size)
{
    char id[BOARD_ID_MAX] = {0};
    if (!board_add(text, cwd, id))
        return 0;
    if (id_out && size > 0)
        snprintf(id_out, (size_t)size, "%s", id);
    return 1;
}

void boardview_run(const char *cwd)
{
    char here[4096];
    snprintf(here, sizeof here, "%s", cwd ? cwd : "");

    char filter[4096];
    snprintf(filter, sizeof filter, "%s", here);

    char sel_id[BOARD_ID_MAX] = {0};

    for (;;) {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);

        struct vlist l = {0};
        int          shown = build_board(&l, cards, n, filter, !filter[0]);
        align(&l);

        if (!l.n) {
            vlist_free(&l);
            board_free(cards, n);
            // A board with nothing in this repo but cards elsewhere is a
            // filter, not an empty board; say which it was.
            if (filter[0]) {
                filter[0] = '\0';
                continue;
            }
            note("the board is empty — /card <text> puts something on it");
            return;
        }

        char where[512] = "all repos";
        if (filter[0])
            path_home_relative(filter, where, sizeof where);

        char title[700];
        if (shown == n)
            snprintf(title, sizeof title, "board · %s · %d card%s", where, n,
                     n == 1 ? "" : "s");
        else
            snprintf(title, sizeof title, "board · %s · %d of %d", where, shown, n);

        int pressed = 0;
        int at = vlist_run(title, &l, row_of(&l, sel_id), BOARD_KEYS, &pressed);
        if (at >= 0)
            snprintf(sel_id, sizeof sel_id, "%s", l.v[at].id);
        vlist_free(&l);

        if (at < 0) {
            board_free(cards, n);
            return;
        }

        struct board_card *c = board_find(cards, n, sel_id);
        switch (pressed) {
        case 0:
            if (c && !detail_run(c->id))
                sel_id[0] = '\0';
            break;
        case KEY_NEW:
            do_new(filter[0] ? filter : here, sel_id);
            break;
        case KEY_MOVE:
            if (c)
                do_move(c);
            break;
        case KEY_EDIT:
            if (c)
                do_edit(c);
            break;
        case KEY_REPO:
            if (c)
                do_repo(c);
            break;
        case KEY_DELETE:
            if (c && do_delete(c))
                sel_id[0] = '\0';
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
