#include "matrix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "chrome.h"
#include "cmd.h"
#include "filediff.h"
#include "frontend.h"
#include "md.h"
#include "models.h"
#include "muxcfg.h"
#include "pick.h"
#include "replbox.h"
#include "session.h"
#include "sessionview.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "title.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"
#include "vendor/agents/backend.h"

#define MATRIX_ROWS MUX_MAX

#define LABEL_MIN 8
#define LABEL_MAX 22
#define BODY_MIN  24

/* a cell narrower than this cannot show a conversation */
#define CELL_MIN 4

#define HIT_MAX 200

#define TARGET_ALL (-1)

enum span_kind { SPAN_SENT, SPAN_SAID, SPAN_THOUGHT, SPAN_TOOL, SPAN_RESULT,
                 SPAN_NOTE };

struct entry {
    enum span_kind kind;
    long           seq;
    int            gap;
    int            failed;
    char          *text;
    char          *name;
    char          *arg;
    char          *diff;

    char          *painted;
    int            painted_width;
};

struct span {
    const char *text;
    size_t      bytes;
    int         width;
};

struct rowbuf {
    struct span *v;
    int          n;
    int          cap;
};

struct cell {
    char name[32];
    char model[128];
    char effort[32];
    char system[MUX_PROMPT];
    char resolved[128];

    struct session *s;
    char            id[128];
    int             adopted;
    int             titled;
    int             failed;

    struct entry *log;
    int           count;
    int           cap;
    int           after_activity;
    int           after_tool;

    struct rowbuf rows;
    int           rows_width;
    int           laid;
    int           scroll; /* rows held back from the tail */
    unsigned      rows_gen;
};

struct view {
    struct cell c[MATRIX_ROWS];
    int         n;
    int         sel; /* TARGET_ALL, or the cell the input talks to */

    char  config[MUX_NAME];
    char *prompt;
    char *head;
    int   head_width;

    struct rowbuf head_rows;

    struct replbox in;
    int            y; /* the screen row the painter is on */
    short          hit[HIT_MAX];
};

/* the rows of the last run, so a bare /mux picks them back up */
static struct mux_spec last_spec[MATRIX_ROWS];
static char            last_id[MATRIX_ROWS][128];
static int             last_n;
static char            last_cwd[1024];

/* Bumped whenever a painted buffer is replaced, so the row cache — which holds
   pointers into those buffers — knows to relay. */
static unsigned paint_gen;

static struct entry *log_add(struct cell *c, enum span_kind kind, const char *text)
{
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        struct entry *grown = realloc(c->log, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        c->log = grown;
        c->cap = cap;
    }
    static long seq;
    struct entry *e = &c->log[c->count];
    *e = (struct entry){.kind = kind, .seq = seq++};
    if (text && !(e->text = strdup(text)))
        return NULL;
    c->count++;
    return e;
}

static void log_free(struct cell *c)
{
    for (int i = 0; i < c->count; i++) {
        free(c->log[i].text);
        free(c->log[i].name);
        free(c->log[i].arg);
        free(c->log[i].diff);
        free(c->log[i].painted);
    }
    free(c->log);
    c->log = NULL;
    c->count = c->cap = c->laid = c->rows.n = 0;
}

static void cell_event(void *ud, const backend_event *ev)
{
    struct cell *c = ud;

    switch (ev->kind) {
    case BACKEND_EV_CWD:
    case BACKEND_EV_TRUST:
    case BACKEND_EV_WARNING:
    case BACKEND_EV_TASK:
        break;

    case BACKEND_EV_INIT:
        if (ev->name && *ev->name)
            snprintf(c->resolved, sizeof c->resolved, "%s", ev->name);
        break;

    case BACKEND_EV_ASSISTANT: {
        if (!ev->text || !*ev->text)
            break;
        struct entry *e = log_add(c, SPAN_SAID, ev->text);
        if (e)
            e->gap = c->after_activity;
        c->after_activity = 0;
        c->after_tool = 0;
        break;
    }

    case BACKEND_EV_THINKING: {
        if (!session_thinking(c->s) || !ev->text || !*ev->text)
            break;
        struct entry *e = log_add(c, SPAN_THOUGHT, ev->text);
        if (e)
            e->gap = c->after_tool;
        c->after_activity = 1;
        c->after_tool = 1;
        break;
    }

    case BACKEND_EV_TOOL: {
        char arg[4096];
        view_tool_argument(ev, session_cwd(c->s), arg, sizeof arg);
        struct entry *e = log_add(c, SPAN_TOOL, NULL);
        if (e) {
            e->gap = c->after_tool;
            e->name = strdup(ev->name ? ev->name : "?");
            e->arg = strdup(arg);
        }
        c->after_activity = 1;
        c->after_tool = 1;
        break;
    }

    case BACKEND_EV_TOOL_RESULT: {
        struct entry *e = log_add(c, SPAN_RESULT, ev->text);
        if (e) {
            e->failed = ev->failed;
            if (ev->diff)
                e->diff = strdup(ev->diff);
        }
        c->after_activity = 1;
        c->after_tool = 1;
        break;
    }
    }
}

static const char *entry_painted(struct entry *e, int width)
{
    if (e->painted && e->painted_width == width)
        return e->painted;

    if (e->painted) {
        free(e->painted);
        e->painted = NULL;
        paint_gen++;
    }
    ui_capture_begin(width);
    if (e->gap)
        ui_put("\n");
    switch (e->kind) {
    case SPAN_SENT:
        ui_wrapped(e->text, 0, UI_ECHO);
        break;
    case SPAN_SAID:
        md_render(e->text, 0);
        ui_put("\n");
        break;
    case SPAN_THOUGHT:
        view_activity("\xe2\x9c\xbb", e->text, UI_THINKING);
        break;
    case SPAN_TOOL:
        view_tool_call(e->name, e->arg);
        break;
    case SPAN_RESULT:
        if (e->failed) {
            const char *why = e->text && *e->text ? e->text : NULL;
            if (!why || !strcmp(why, "failed")) {
                view_tool_output("failed", UI_ERROR);
            } else {
                char line[4096];
                snprintf(line, sizeof line, "failed: %s", why);
                view_tool_error(line);
            }
        } else if (!e->diff || !filediff_render_patch(e->diff)) {
            view_tool_output(e->text, UI_DIM);
        }
        break;
    case SPAN_NOTE:
        ui_wrapped(e->text, 0, UI_ERROR);
        break;
    }
    e->painted = ui_capture_end();
    e->painted_width = width;
    return e->painted;
}

static void rowbuf_add(struct rowbuf *rb, const char *text, size_t bytes, int width)
{
    if (ui_cells_visible(text, bytes) > (size_t)width)
        bytes = ui_fit_visible(text, bytes, (size_t)width);

    if (rb->n == rb->cap) {
        int cap = rb->cap ? rb->cap * 2 : 64;
        struct span *grown = realloc(rb->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        rb->v = grown;
        rb->cap = cap;
    }
    rb->v[rb->n++] = (struct span){text, bytes, (int)ui_cells_visible(text, bytes)};
}

static void rowbuf_split(struct rowbuf *rb, const char *painted, int width)
{
    const char *p = painted;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        rowbuf_add(rb, p, nl ? (size_t)(nl - p) : strlen(p), width);
        if (!nl)
            break;
        p = nl + 1;
    }
}

static void cell_lay(struct cell *c, int width)
{
    if (c->rows_width != width || c->rows_gen != paint_gen) {
        c->rows_width = width;
        c->rows.n = 0;
        c->laid = 0;
    }

    int count = c->count;

    for (; c->laid < count; c->laid++)
        rowbuf_split(&c->rows, entry_painted(&c->log[c->laid], width), width);
    c->rows_gen = paint_gen;
}

static void head_lay(struct view *v, int width)
{
    if (v->head && v->head_width == width)
        return;

    free(v->head);
    v->head_rows.n = 0;
    v->head_width = width;
    ui_capture_begin(width);
    ui_wrapped(v->prompt ? v->prompt : "", 0, UI_ECHO);
    v->head = ui_capture_end();
    rowbuf_split(&v->head_rows, v->head, width);
}

static void board_lay(struct view *v, int width)
{
    head_lay(v, width);
    for (int i = 0; i < v->n; i++)
        cell_lay(&v->c[i], width);
}

static int cell_labels(const struct view *v, int at, char out[4][64],
                       enum ui_role *role)
{
    const struct cell *c = &v->c[at];
    int                n = 0;

    role[n] = c->failed  ? UI_ERROR
              : v->sel == at ? UI_ACCENT
                             : UI_BOLD;
    snprintf(out[n++], 64, "%d %s", at + 1, c->name);

    const char *model = c->s ? session_model(c->s) : NULL;
    if (!model || !*model)
        model = *c->resolved ? c->resolved : *c->model ? c->model : "default";
    model = models_short_name(c->name, model);
    role[n] = UI_DIM;
    snprintf(out[n++], 64, "%s", model);

    const char *effort = c->s ? session_effort(c->s) : c->effort;
    if (effort && *effort && strcmp(effort, "default")) {
        role[n] = UI_DIM;
        snprintf(out[n++], 64, "%s", effort);
    }

    if (c->s) {
        const char *word = workspace_status(c->s);
        role[n] = !strcmp(word, "working") ? UI_ACCENT
                  : !strcmp(word, "errored") ? UI_ERROR
                                             : UI_DIM;
        snprintf(out[n++], 64, "%s", word);
    }
    return n;
}

static int label_width(const struct view *v, int budget)
{
    int width = LABEL_MIN;

    for (int i = 0; i < v->n; i++) {
        char         text[4][64];
        enum ui_role role[4];
        int          n = cell_labels(v, i, text, role);
        for (int r = 0; r < n; r++) {
            int cells = (int)ui_cells(text[r]);
            if (cells > width)
                width = cells;
        }
    }
    if (width > LABEL_MAX)
        width = LABEL_MAX;
    return width > budget ? budget : width;
}

static void rule(struct view *v, int labelw, int bodyw, const char *left,
                 const char *mid, const char *right)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(left);
    for (int i = 0; i < labelw + 2; i++)
        ui_put("\xe2\x94\x80");
    ui_put(mid);
    for (int i = 0; i < bodyw + 2; i++)
        ui_put("\xe2\x94\x80");
    ui_put(right);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    v->y++;
}

static void edge(void)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put("\xe2\x94\x82");
    ui_esc(ui_style(UI_RESET));
}

static void table_row(struct view *v, int at, int labelw, int bodyw,
                      const char *label, enum ui_role role, const struct span *c)
{
    if (at >= 0 && v->y >= 0 && v->y < HIT_MAX)
        v->hit[v->y] = (short)at;
    v->y++;

    edge();
    ui_put(" ");
    if (label && *label) {
        size_t bytes = ui_fit_bytes(label, (size_t)labelw);
        ui_esc(ui_style(role));
        ui_putn(label, bytes);
        ui_esc(ui_style(UI_RESET));
        ui_pad(labelw - (int)ui_cells_n(label, bytes));
    } else {
        ui_pad(labelw);
    }
    ui_put(" ");

    edge();
    ui_put(" ");
    if (c) {
        ui_putn(c->text, c->bytes);
        ui_esc(ui_style(UI_RESET));
        ui_pad(bodyw - c->width);
    } else {
        ui_pad(bodyw);
    }
    ui_put(" ");
    edge();
    ui_put("\n");
}

static void head_block(struct view *v, int labelw, int bodyw)
{
    int rows = v->head_rows.n > 0 ? v->head_rows.n : 1;

    for (int r = 0; r < rows; r++)
        table_row(v, -1, labelw, bodyw, r ? NULL : v->config, UI_DIM,
                  r < v->head_rows.n ? &v->head_rows.v[r] : NULL);
}

static int standing_row(const struct cell *c, int bodyw, char *out, size_t cap,
                        struct span *pin)
{
    if (!*c->system)
        return 0;

    char text[MUX_PROMPT + 8];
    snprintf(text, sizeof text, "\xe2\x80\xba %s", c->system);

    size_t bytes = ui_fit_bytes(text, (size_t)(bodyw > 1 ? bodyw - 1 : 1));
    snprintf(out, cap, "%s%.*s%s%s", ui_style(UI_DIM), (int)bytes, text,
             text[bytes] ? "\xe2\x80\xa6" : "", ui_style(UI_RESET));

    size_t n = strlen(out);
    *pin = (struct span){out, n, (int)ui_cells_visible(out, n)};
    return 1;
}

static int cell_block(struct view *v, int at, int labelw, int bodyw, int cap)
{
    struct cell *c = &v->c[at];

    char         label[4][64];
    enum ui_role role[4];
    int          labels = cell_labels(v, at, label, role);

    char        note[MUX_PROMPT + 64];
    struct span pin;
    int         pinned = standing_row(c, bodyw, note, sizeof note, &pin);

    int from = 0, rows = c->rows.n;
    if (cap && rows > cap - pinned) {
        from = rows - (cap - pinned);
        rows = cap - pinned;
    }
    if (rows < 0)
        rows = 0;

    if (c->scroll > from)
        c->scroll = from;
    if (c->scroll < 0)
        c->scroll = 0;
    from -= c->scroll;

    int height = rows + pinned > labels ? rows + pinned : labels;
    if (cap > 0 && height > cap)
        height = cap;
    if (height < 1)
        height = 1;

    for (int r = 0; r < height; r++) {
        const struct span *s = NULL;
        if (pinned && r == 0)
            s = &pin;
        else if (r - pinned < rows)
            s = &c->rows.v[from + r - pinned];

        table_row(v, at, labelw, bodyw, r < labels ? label[r] : NULL,
                  r < labels ? role[r] : UI_DIM, s);
    }
    return height;
}

static void board_stacked(struct view *v, int cols)
{
    for (int i = 0; i < v->n; i++) {
        struct cell *c = &v->c[i];
        char         label[4][64];
        enum ui_role role[4];
        int          labels = cell_labels(v, i, label, role);

        ui_bar(ui_style(UI_CHROME), "%s \xc2\xb7 %s", label[0],
               labels > 1 ? label[1] : "");
        ui_put("\n");
        if (*c->system) {
            char        note[MUX_PROMPT + 64];
            struct span pin;
            if (standing_row(c, cols, note, sizeof note, &pin)) {
                ui_putn(pin.text, pin.bytes);
                ui_put("\n");
            }
        }
        for (int e = 0; e < c->count; e++) {
            const char *painted = entry_painted(&c->log[e], cols);
            if (painted)
                ui_put(painted);
        }
        ui_put("\n");
    }
}

/* the run as it is left behind in the scrollback, and as a headless /mux
   prints it */
static void board_render(void *ud, int cols)
{
    struct view *v = ud;

    int total = cols - 1;
    int labelw = label_width(v, total / 3);
    int bodyw = total - labelw - 7;

    if (bodyw < BODY_MIN || labelw < LABEL_MIN) {
        board_stacked(v, cols);
        return;
    }

    board_lay(v, bodyw);

    v->y = -1;
    rule(v, labelw, bodyw, "\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");
    head_block(v, labelw, bodyw);
    for (int i = 0; i < v->n; i++) {
        rule(v, labelw, bodyw, "\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
        cell_block(v, i, labelw, bodyw, 0);
    }
    rule(v, labelw, bodyw, "\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");
}

static void board_free(void *ud)
{
    struct view *v = ud;
    for (int i = 0; i < v->n; i++) {
        free(v->c[i].rows.v);
        log_free(&v->c[i]);
    }
    free(v->head_rows.v);
    free(v->head);
    free(v->prompt);
    free(v);
}

/* ---- the view ---- */

/* Nothing here stops a cell mid-turn: the interrupt is ^c at the prompt, and
   ^c in the view already means leave. The second line says where to go. */
static const char HINT[] =
    "enter send  \xc2\xb7  \xe2\x86\x91\xe2\x86\x93 pick a cell  \xc2\xb7  esc all  \xc2\xb7  ^c leave\n"
    "enter on an empty line opens the cell as a tab, where its turn can be stopped";

static void target_prefix(const struct view *v, char *out, size_t cap)
{
    if (v->sel < 0)
        snprintf(out, cap, "all \xe2\x80\xba ");
    else
        snprintf(out, cap, "%d %s \xe2\x80\xba ", v->sel + 1, v->c[v->sel].name);
}

/* the selected cell takes the rows the others do not need */
static void heights(const struct view *v, int body, int *out)
{
    int n = v->n;
    if (n < 1)
        return;

    if (body < n * CELL_MIN) {
        int each = body / n;
        if (each < 1)
            each = 1;
        for (int i = 0; i < n; i++)
            out[i] = each;
        return;
    }

    if (v->sel < 0 || n == 1) {
        int each = body / n, extra = body % n;
        for (int i = 0; i < n; i++)
            out[i] = each + (i < extra);
        return;
    }

    int rest = n - 1;
    int want = body / 2;
    if (want < CELL_MIN)
        want = CELL_MIN;
    if (body - want < rest * CELL_MIN)
        want = body - rest * CELL_MIN;

    int each = (body - want) / rest, extra = (body - want) % rest;
    for (int i = 0, k = 0; i < n; i++) {
        if (i == v->sel) {
            out[i] = want;
            continue;
        }
        out[i] = each + (k < extra);
        k++;
    }
}

static int input_cols(const struct view *v, int columns)
{
    char prefix[64];
    target_prefix(v, prefix, sizeof prefix);
    int budget = columns - (int)ui_cells(prefix) - 2;
    return budget < 8 ? 8 : budget;
}

static void paint_input(struct view *v, int columns)
{
    char prefix[64];
    target_prefix(v, prefix, sizeof prefix);
    int pad = (int)ui_cells(prefix);

    replbox_width(&v->in, input_cols(v, columns) + 2);
    int rows = replbox_wants(&v->in);
    if (rows > 5)
        rows = 5;
    if (!replbox_render(&v->in, replbox_wants(&v->in)))
        return;
    replbox_scroll(&v->in, rows);

    int top = replbox_top(&v->in);
    for (int r = 0; r < rows; r++) {
        if (r == 0) {
            ui_esc(ui_style(v->sel < 0 ? UI_DIM : UI_ACCENT));
            ui_put(prefix);
            ui_esc(ui_style(UI_RESET));
        } else {
            ui_pad(pad);
        }
        replbox_paint_row(&v->in, top + r, 2, 1);
        ui_put("\n");
        v->y++;
    }
}

static int input_rows(struct view *v, int columns)
{
    replbox_width(&v->in, input_cols(v, columns) + 2);
    int rows = replbox_wants(&v->in);
    return rows > 5 ? 5 : rows;
}

static void paint(void *ud)
{
    struct view *v = ud;
    int          columns = ui_columns();

    for (int i = 0; i < HIT_MAX; i++)
        v->hit[i] = -1;

    char title[128];
    snprintf(title, sizeof title, "matrix \xc2\xb7 %s", v->config);

    v->y = chrome_gap();
    chrome_title_paint(title);
    v->y++;

    int total = columns - 1;
    int labelw = label_width(v, total / 3);
    int bodyw = total - labelw - 7;
    if (bodyw < BODY_MIN)
        bodyw = BODY_MIN;
    if (labelw < LABEL_MIN)
        labelw = LABEL_MIN;

    board_lay(v, bodyw);

    int head = v->head_rows.n > 0 ? v->head_rows.n : 1;
    int foot = chrome_foot_rows(NULL, HINT, columns);
    int spent = 1 + head + v->n + 2 + foot + input_rows(v, columns) + 1;
    int body = chrome_modal_rows() - spent;
    if (body < v->n)
        body = v->n;

    int cap[MATRIX_ROWS] = {0};
    heights(v, body, cap);

    rule(v, labelw, bodyw, "\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");
    head_block(v, labelw, bodyw);
    for (int i = 0; i < v->n; i++) {
        rule(v, labelw, bodyw, "\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
        cell_block(v, i, labelw, bodyw, cap[i]);
    }
    rule(v, labelw, bodyw, "\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");

    ui_put("\n");
    v->y++;
    paint_input(v, columns);
    chrome_foot_paint(NULL, HINT, columns);
}

static void note(struct cell *c, const char *text)
{
    log_add(c, SPAN_NOTE, text);
}

static void sent(struct cell *c, const char *text)
{
    struct entry *e = log_add(c, SPAN_SENT, text);
    if (e)
        e->gap = 1;
    c->after_activity = 0;
    c->after_tool = 0;
}

static void send_to(struct view *v, int at, const char *line)
{
    struct cell *c = &v->c[at];
    if (!c->s) {
        note(c, "this row never started");
        return;
    }
    sent(c, line);
    cmd_submit(c->s, line);
}

static void send_all(struct view *v, const char *line)
{
    free(v->prompt);
    v->prompt = strdup(line);
    free(v->head);
    v->head = NULL;
    v->head_width = 0;

    for (int i = 0; i < v->n; i++)
        send_to(v, i, line);
}

/* a cell's id only exists once it has taken a turn: that is when the row can
   be named, and when it becomes something a later /mux can reopen */
static void catch_ids(struct view *v)
{
    for (int i = 0; i < v->n; i++) {
        struct cell *c = &v->c[i];
        if (c->titled || !c->s)
            continue;
        const char *id = session_id(c->s);
        if (!id || !*id)
            continue;
        snprintf(c->id, sizeof c->id, "%s", id);

        char label[160];
        snprintf(label, sizeof label, "matrix \xc2\xb7 %s \xc2\xb7 %s", c->name,
                 *c->system ? c->system : (v->prompt ? v->prompt : ""));
        /* title_set refuses a name over 80 bytes rather than clipping it, and
           a cell budget is not a byte budget once the text is not ascii */
        size_t cells = 60, fit = ui_fit_bytes(label, cells);
        while (fit > 78 && cells > 8) {
            cells -= 4;
            fit = ui_fit_bytes(label, cells);
        }
        label[fit] = '\0';
        title_set(c->id, label);
        c->titled = 1;
    }
}

static void keep(struct view *v, const char *cwd)
{
    last_n = 0;
    snprintf(last_cwd, sizeof last_cwd, "%s", cwd ? cwd : "");
    for (int i = 0; i < v->n; i++) {
        struct cell *c = &v->c[i];
        if (!*c->id || c->adopted)
            continue;
        struct mux_spec *m = &last_spec[last_n];
        snprintf(m->backend, sizeof m->backend, "%s", c->name);
        snprintf(m->model, sizeof m->model, "%s", c->model);
        snprintf(m->effort, sizeof m->effort, "%s", c->effort);
        snprintf(m->prompt, sizeof m->prompt, "%s", c->system);
        snprintf(last_id[last_n], sizeof last_id[0], "%s", c->id);
        last_n++;
    }
}

/* The idle rows nobody kept go; the rest stay as ordinary tabs. Either way the
   view drops its session pointers, because what it paints into the scrollback
   outlives every one of them. */
static void retire(struct view *v)
{
    for (int i = 0; i < v->n; i++) {
        struct cell *c = &v->c[i];
        if (!c->s)
            continue;
        session_set_observer(c->s, NULL, NULL);
        if (!c->adopted && !session_turn_running(c->s)) {
            int at = workspace_index_of(c->s);
            if (at > 0)
                workspace_close(at);
        }
        c->s = NULL;
    }
}

static int scroll_cell(struct view *v, int by)
{
    if (v->sel < 0)
        return 0;
    v->c[v->sel].scroll += by;
    if (v->c[v->sel].scroll < 0)
        v->c[v->sel].scroll = 0;
    return 1;
}

static void step(struct view *v, int delta)
{
    if (v->n < 1)
        return;
    if (v->sel < 0) {
        v->sel = delta < 0 ? v->n - 1 : 0;
        return;
    }
    int want = v->sel + delta;
    if (want < 0 || want >= v->n)
        return;
    v->sel = want;
}

/* the selected cell, taken over as the front tab. The index is not held on
   to: retiring the other rows shifts every tab after them. */
static struct session *adopt(struct view *v)
{
    if (v->sel < 0 || !v->c[v->sel].s)
        return NULL;
    v->c[v->sel].adopted = 1;
    return v->c[v->sel].s;
}

static struct session *view_run(struct view *v)
{
    struct session *leave_to = NULL;

    replbox_init(&v->in, NULL, 0);
    chrome_full(1);
    chrome_modal(paint, v);

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, PICK_POLL_MS)) {
            if (chrome_modal_interrupted())
                break;
            workspace_pump_quiet();
            catch_ids(v);
            chrome_paint();
            continue;
        }

        workspace_pump_quiet();
        catch_ids(v);

        int empty = !*replbox_line(&v->in);

        switch (ev.key) {
        case TK_ENTER:
            if (empty) {
                leave_to = adopt(v);
                goto done;
            }
            {
                char *line = strdup(replbox_line(&v->in));
                replbox_reset(&v->in);
                if (v->sel < 0)
                    send_all(v, line);
                else
                    send_to(v, v->sel, line);
                free(line);
            }
            break;

        case TK_ESCAPE:
            if (v->sel < 0)
                goto done;
            v->sel = TARGET_ALL;
            break;

        case TK_EOF:
            goto done;

        case TK_UP:
        case TK_DOWN:
            if (empty)
                step(v, ev.key == TK_UP ? -1 : 1);
            else
                replbox_key(&v->in, &ev);
            break;

        case TK_SCROLL_UP:
            scroll_cell(v, 3);
            break;
        case TK_SCROLL_DOWN:
            scroll_cell(v, -3);
            break;
        case TK_PAGE_UP:
            scroll_cell(v, 10);
            break;
        case TK_PAGE_DOWN:
            scroll_cell(v, -10);
            break;

        case TK_MOUSE_DOWN: {
            int top = viewport_chrome_top();
            if (top < 0)
                break;
            int at = ev.row - 1 - top;
            if (at < 0 || at >= HIT_MAX || v->hit[at] < 0)
                break;
            if (v->hit[at] != v->sel) {
                v->sel = v->hit[at];
                break;
            }
            leave_to = adopt(v);
            goto done;
        }

        case TK_RESIZE:
            break;

        default:
            if (ev.key == TK_CHAR && (ev.cp == 3 || ev.cp == 4) && empty) {
                free(ev.text);
                goto done;
            }
            replbox_key(&v->in, &ev);
            free(ev.text);
            break;
        }
        chrome_paint();
    }

done:
    chrome_modal(NULL, NULL);
    chrome_full(0);
    replbox_free(&v->in);
    return leave_to;
}

/* ---- opening the rows ---- */

static int open_cells(struct view *v, struct session *s, const char *cwd,
                      const struct mux_spec *spec, int n, char ids[][128])
{
    int free_tabs = WORKSPACE_MAX - workspace_count();
    int         dropped = 0;

    if (n > free_tabs) {
        dropped = n - free_tabs;
        n = free_tabs > 0 ? free_tabs : 0;
    }

    for (int i = 0; i < n; i++) {
        struct cell *c = &v->c[v->n];
        snprintf(c->name, sizeof c->name, "%s", spec[i].backend);
        snprintf(c->model, sizeof c->model, "%s", spec[i].model);
        snprintf(c->effort, sizeof c->effort, "%s", spec[i].effort);
        snprintf(c->system, sizeof c->system, "%s", spec[i].prompt);
        if (ids && *ids[i])
            snprintf(c->id, sizeof c->id, "%s", ids[i]);
        v->n++;

        if (*c->id && workspace_find_id(c->id) >= 0) {
            note(c, "this row is already open as a tab");
            continue;
        }

        int at = workspace_spawn_ex(c->name, spec[i].model, spec[i].effort, cwd,
                                    *c->id ? c->id : NULL, spec[i].prompt);
        if (at < 0) {
            c->failed = 1;
            note(c, "could not open this row");
            continue;
        }
        c->s = workspace_at(at);
        session_set_naming(c->s, 0);
        /* a session that has taken no turn has no id yet: the rows open all
           the same and simply hang under nothing */
        session_set_parent(c->s, session_id(s));
        session_set_permission(c->s, session_permission(s));
        session_set_thinking(c->s, session_thinking(s));
        session_set_observer(c->s, cell_event, c);
        if (*c->id)
            c->titled = 1;
    }

    if (dropped)
        ui_error("%d row%s left out \xe2\x80\x94 the window holds %d tabs",
                 dropped, dropped == 1 ? "" : "s", WORKSPACE_MAX);
    return v->n;
}

/* /mux over telegram or a pipe: the same cells, no view */
static int run_headless(struct view *v)
{
    status_set_word("fanning out");
    status_begin();

    while (workspace_busy()) {
        workspace_pump_quiet();
        catch_ids(v);
        status_tick();
    }
    status_end();
    catch_ids(v);

    int answered = 0;
    for (int i = 0; i < v->n; i++)
        answered += v->c[i].count > 1;
    return answered;
}

static void leave_behind(struct view *v)
{
    if (viewport_active()) {
        viewport_item_begin(&(struct viewport_entry){.render = board_render,
                                                     .ud = v,
                                                     .free_ud = board_free,
                                                     .reflow = 1,
                                                     .pad_before = 1,
                                                     .pad_after = 1});
        board_render(v, ui_columns());
        viewport_item_end();
    } else {
        board_render(v, ui_columns());
        ui_flush();
        board_free(v);
    }
}

static int go(struct session *s, const char *cwd, const struct mux_spec *spec,
              int n, char ids[][128], const char *prompt)
{
    struct view *v = calloc(1, sizeof *v);
    if (!v)
        return 0;

    v->sel = TARGET_ALL;
    v->prompt = strdup(prompt ? prompt : "");
    snprintf(v->config, sizeof v->config, "%s", muxcfg_active());

    if (!open_cells(v, s, cwd, spec, n, ids)) {
        board_free(v);
        return 0;
    }

    if (prompt && *prompt)
        for (int i = 0; i < v->n; i++)
            send_to(v, i, prompt);

    int answered;
    if (!frontend_has_keyboard() || !tty_is_raw() || ui_too_narrow()) {
        answered = run_headless(v);
        keep(v, cwd);
        retire(v);
        leave_behind(v);
        return answered;
    }

    struct session *leave_to = view_run(v);
    keep(v, cwd);
    retire(v);

    answered = 0;
    for (int i = 0; i < v->n; i++)
        answered += v->c[i].count > 1;

    leave_behind(v);
    if (leave_to) {
        int at = workspace_index_of(leave_to);
        if (at >= 0)
            workspace_show(at);
    }
    return answered;
}

int matrix_run(struct session *s, const char *prompt)
{
    if (!prompt || !*prompt)
        return 0;

    struct mux_spec spec[MATRIX_ROWS];
    int             n = muxcfg_load(spec, MATRIX_ROWS);
    if (n < 1) {
        ui_error("the mux matrix is empty \xe2\x80\x94 /mux config to fill it in");
        ui_put("\n");
        ui_flush();
        return 0;
    }
    return go(s, session_cwd(s), spec, n, NULL, prompt);
}

int matrix_reopen(struct session *s)
{
    if (last_n < 1)
        return 0;

    struct mux_spec spec[MATRIX_ROWS];
    char            ids[MATRIX_ROWS][128];
    int             n = last_n;

    memcpy(spec, last_spec, (size_t)n * sizeof *spec);
    memcpy(ids, last_id, (size_t)n * sizeof ids[0]);

    /* the rows reopen where they ran, not where the session is now, or the
       CLI will not find the transcript they wrote */
    char cwd[sizeof last_cwd];
    snprintf(cwd, sizeof cwd, "%s", *last_cwd ? last_cwd : session_cwd(s));

    go(s, cwd, spec, n, ids, NULL);
    return 1;
}
