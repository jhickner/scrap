#include "sessionview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "filediff.h"
#include "highlight.h"
#include "scrollback.h"
#include "text.h"
#include "toolstyle.h"
#include "viewport.h"
#include "vendor/cJSON.h"

#define TOOL_INDENT 2

#define NEST_MARK "\xe2\x86\xb3"

static int         nest;
static const char *nest_label;
static int         nest_said;

static int nest_width(const char *label)
{
    int n = (int)ui_cells(NEST_MARK) + 1;
    if (label && *label)
        n += (int)ui_cells(label) + 1;
    return n;
}

static void nest_pad(int base)
{
    ui_pad(base);
    if (!nest)
        return;
    if (nest_said) {
        ui_pad(nest);
        return;
    }
    nest_said = 1;
    ui_esc(ui_style(UI_CHROME));
    ui_put(NEST_MARK);
    if (nest_label && *nest_label) {
        ui_put(" ");
        ui_put(nest_label);
    }
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
}

enum keep_kind { KEEP_ACTIVITY, KEEP_CALL, KEEP_OUTPUT, KEEP_DIFF };

struct keep {
    enum keep_kind kind;
    char          *a;
    char          *b;
    unsigned char *spans;
    enum ui_role   role;
    int            error;
    int            collapses;
    int            background;
    int            nested;
    char          *label;
    char          *row;
    char          *call;
};

static void keep_free(void *ud)
{
    struct keep *k = ud;
    free(k->a);
    free(k->b);
    free(k->spans);
    free(k->label);
    free(k->row);
    free(k->call);
    free(k);
}

static void cluster_paint(const char *line, const unsigned char *spans);
static void tool_tag(const char *name, int background, char *out, size_t size);
static void tool_call(const char *name, const char *arg, int background);
static unsigned char *row_spans(const char *name, const char *row, size_t prefix);
static int cluster_budget(void);
static void view_activity(const char *marker, const char *text, enum ui_role role);
static void view_tool_output(const char *text, enum ui_role role);

struct sessionview_state {
    int      collapsed;
    unsigned run_start;
};

static char state_owner;

static struct sessionview_state *view_state(void)
{
    static struct sessionview_state fallback;
    struct sessionview_state *st =
        viewport_state_local(&state_owner, sizeof(struct sessionview_state));
    return st ? st : &fallback;
}

int view_collapsed(void) { return view_state()->collapsed; }

static int keep_drops(const struct keep *k)
{
    return k->kind == KEEP_DIFF || (k->kind == KEEP_OUTPUT && !k->error);
}

static size_t row_prefix(const char *row)
{
    size_t n = strcspn(row, "]");
    return row[n] ? n + 2 : n;
}

static void call_row(const char *name, const char *arg, int background, char *out,
                     size_t size)
{
    char tag[64];
    tool_tag(name, background, tag, sizeof tag);

    char flat[4096];
    text_one_line(arg ? arg : "", flat, sizeof flat);

    snprintf(out, size, "%s %s", tag, flat);
}

static int call_row_extend(const char *base, const char *arg, char *out, size_t size)
{
    if (!base)
        return 0;
    if (!arg || !*arg) {
        snprintf(out, size, "%s", base);
        return 1;
    }

    char flat[4096];
    text_one_line(arg, flat, sizeof flat);

    snprintf(out, size, "%s, %s", base, flat);
    return (int)ui_cells(out) <= cluster_budget();
}

static void call_collapsed(const struct keep *k)
{
    char own[4096];
    const char *row = k->row;
    if (!row) {
        call_row(k->a, k->b, k->background, own, sizeof own);
        row = own;
    }

    unsigned char *spans = row_spans(k->a, row, row_prefix(row));
    cluster_paint(row, spans);
    free(spans);
}

static void keep_render(void *ud, int cols)
{
    const struct keep *k = ud;
    (void)cols;

    nest_label = k->nested ? k->label : NULL;
    nest = k->nested ? nest_width(nest_label) : 0;
    nest_said = 0;
    if (view_state()->collapsed && keep_drops(k)) {
        nest = 0;
        return;
    }
    if (k->kind == KEEP_CALL && k->spans) {
        cluster_paint(k->a, k->spans);
        nest = 0;
        return;
    }
    if (k->kind == KEEP_CALL && (view_state()->collapsed || k->collapses)) {
        call_collapsed(k);
        nest = 0;
        return;
    }

    switch (k->kind) {
    case KEEP_ACTIVITY: view_activity(k->a, k->b, k->role);              break;
    case KEEP_CALL:     tool_call(k->a, k->b, k->background);            break;
    case KEEP_OUTPUT:
        if (k->error)
            view_tool_error(k->a);
        else
            view_tool_output(k->a, k->role);
        break;
    case KEEP_DIFF:     filediff_render_patch(k->a);                     break;
    }
    nest = 0;
}

static const char HEX[] = "0123456789abcdef";

static char *spans_hex(const unsigned char *spans, size_t n)
{
    char *out = malloc(n * 2 + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = HEX[spans[i] >> 4];
        out[i * 2 + 1] = HEX[spans[i] & 0xf];
    }
    out[n * 2] = '\0';
    return out;
}

static unsigned char *hex_spans(const char *hex, size_t want)
{
    if (!hex || strlen(hex) != want * 2)
        return NULL;
    unsigned char *out = malloc(want ? want : 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < want; i++) {
        const char *hi = strchr(HEX, hex[i * 2]), *lo = strchr(HEX, hex[i * 2 + 1]);
        if (!hi || !lo) {
            free(out);
            return NULL;
        }
        out[i] = (unsigned char)((hi - HEX) << 4 | (lo - HEX));
    }
    return out;
}

static char *keep_encode(void *ud)
{
    const struct keep *k = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddNumberToObject(o, "kind", k->kind);
    cJSON_AddStringToObject(o, "a", k->a ? k->a : "");
    cJSON_AddStringToObject(o, "b", k->b ? k->b : "");
    cJSON_AddNumberToObject(o, "role", k->role);
    cJSON_AddNumberToObject(o, "error", k->error);
    cJSON_AddNumberToObject(o, "collapses", k->collapses);
    cJSON_AddNumberToObject(o, "background", k->background);
    if (k->nested) {
        cJSON_AddNumberToObject(o, "nested", k->nested);
        if (k->label)
            cJSON_AddStringToObject(o, "label", k->label);
    }
    if (k->spans && k->a) {
        char *hex = spans_hex(k->spans, strlen(k->a));
        if (hex) {
            cJSON_AddStringToObject(o, "spans", hex);
            free(hex);
        }
    }
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static void restate(unsigned from, int stale);

static unsigned keep(struct keep *k)
{
    unsigned mark = viewport_item_begin(&(struct viewport_entry){
        .render = keep_render, .ud = k, .free_ud = keep_free, .reflow = 1,
        .pad_before = 1});
    keep_render(k, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, VIEW_KEEP_KIND, keep_encode);
    if (mark)
        restate(view_state()->run_start ? view_state()->run_start : mark, 0);
    return mark;
}

static int   nesting;
static char *nesting_label;

void view_keep_nest(int on, const char *label)
{
    nesting = on ? 1 : 0;
    free(nesting_label);
    nesting_label = on && label && *label ? strdup(label) : NULL;
}

static struct keep *keep_new(enum keep_kind kind)
{
    struct keep *k = calloc(1, sizeof *k);
    if (k) {
        k->kind = kind;
        k->nested = nesting;
        k->label = nesting_label ? strdup(nesting_label) : NULL;
    }
    return k;
}

void view_keep_load(const cJSON *st)
{
    int kind = scrollback_int(st, "kind");

    enum { KEEP_CLUSTER = 4 };
    if (kind == KEEP_CLUSTER)
        kind = KEEP_CALL;
    if (kind < KEEP_ACTIVITY || kind > KEEP_DIFF)
        return;

    struct keep *k = keep_new((enum keep_kind)kind);
    if (!k)
        return;
    k->nested = scrollback_int(st, "nested");
    const char *label = scrollback_str(st, "label");
    if (*label) {
        free(k->label);
        k->label = strdup(label);
    }
    k->a = strdup(scrollback_str(st, "a"));
    k->b = strdup(scrollback_str(st, "b"));
    k->role = (enum ui_role)scrollback_int(st, "role");
    k->error = scrollback_int(st, "error");
    k->collapses = scrollback_int(st, "collapses");
    k->background = scrollback_int(st, "background");
    if (!k->a || !k->b) {
        keep_free(k);
        return;
    }
    const char *hex = scrollback_str(st, "spans");
    if (*hex)
        k->spans = hex_spans(hex, strlen(k->a));
    keep(k);
}

void view_keep_activity(const char *marker, const char *text, enum ui_role role)
{
    struct keep *k = keep_new(KEEP_ACTIVITY);
    if (!k)
        return;
    k->a = strdup(marker ? marker : "");
    k->b = strdup(text ? text : "");
    k->role = role;
    keep(k);
}

void view_keep_tool_call(const char *name, const char *arg, int collapses)
{
    view_keep_tool_call_bg(name, arg, collapses, 0, NULL);
}

void view_keep_tool_call_bg(const char *name, const char *arg, int collapses, int background,
                            const char *call)
{
    struct keep *k = keep_new(KEEP_CALL);
    if (!k)
        return;
    k->background = background;
    k->a = strdup(name ? name : "?");
    k->b = strdup(arg ? arg : "");
    k->collapses = collapses;
    k->call = call && *call ? strdup(call) : NULL;
    keep(k);
}

static void background_item(unsigned mark, const char *kind, void *ud, void *ctx)
{
    struct keep *k = ud;
    if (!ud || !kind || strcmp(kind, VIEW_KEEP_KIND) != 0 || k->kind != KEEP_CALL ||
        !k->call || strcmp(k->call, ctx) != 0 || k->background)
        return;
    k->background = 1;
    free(k->row);
    k->row = NULL;
    viewport_item_stale(mark);
}

void view_keep_background(const char *call)
{
    if (!call || !*call)
        return;
    viewport_scan(0, background_item, (void *)call);
    restate(0, 0);
}

void view_keep_break(void) { view_state()->run_start = 0; }

void view_keep_output(const char *text, enum ui_role role, int error)
{
    if (!text || !*text)
        return;
    struct keep *k = keep_new(KEEP_OUTPUT);
    if (!k)
        return;
    k->a = strdup(text);
    k->role = role;
    k->error = error;
    keep(k);
}

void view_keep_diff(char *patch)
{
    if (!patch || !*patch) {
        free(patch);
        return;
    }
    struct keep *k = keep_new(KEEP_DIFF);
    if (!k) {
        free(patch);
        return;
    }
    k->a = patch;
    keep(k);
}

struct collapse {
    int          stale;
    struct keep *head;
    unsigned     head_mark;
};

static void row_set(struct keep *k, unsigned mark, const char *row)
{
    if (k->row && strcmp(k->row, row) == 0)
        return;
    free(k->row);
    k->row = strdup(row);
    viewport_item_stale(mark);
}

static void restate_item(unsigned mark, const char *kind, void *ud, void *ctx)
{
    struct collapse *c = ctx;

    if (!ud || !kind || strcmp(kind, VIEW_KEEP_KIND) != 0) {
        c->head = NULL;
        c->head_mark = 0;
        return;
    }
    struct keep *k = ud;

    if (view_state()->collapsed && keep_drops(k)) {
        viewport_item_hide(mark, 1);
        if (c->stale)
            viewport_item_stale(mark);
        return;
    }

    int as_row = k->kind == KEEP_CALL && (view_state()->collapsed || k->collapses);

    char row[4096];
    if (as_row && c->head && c->head->collapses == k->collapses &&
        c->head->background == k->background &&
        strcmp(c->head->a, k->a) == 0 &&
        call_row_extend(c->head->row, k->b, row, sizeof row)) {
        row_set(c->head, c->head_mark, row);
        viewport_item_hide(mark, 1);
        return;
    }

    viewport_item_hide(mark, 0);

    viewport_item_pad(mark, !view_state()->collapsed);
    if (c->stale)
        viewport_item_stale(mark);

    if (as_row) {
        call_row(k->a, k->b, k->background, row, sizeof row);
        row_set(k, mark, row);
        c->head = k;
        c->head_mark = mark;
        return;
    }

    c->head = NULL;
    c->head_mark = 0;
}

static void restate(unsigned from, int stale)
{
    struct collapse c = {.stale = stale};
    viewport_scan(from, restate_item, &c);
    view_state()->run_start = c.head_mark;
    viewport_repad();
}

static void rewidth(void) { restate(0, 0); }

void view_collapse(int on)
{
    int want = on ? 1 : 0;
    viewport_on_width(rewidth);
    if (view_state()->collapsed != want) {
        view_state()->collapsed = want;
        restate(0, 1);
    }
    viewport_paint();
}

static const struct {
    const char *key;
    int         is_path;
} TOOL_ARG_KEYS[] = {
    {"command", 0},       {"file_path", 1},        {"target_file", 1},
    {"notebook_path", 1}, {"path", 1},             {"target_directory", 0},
    {"pattern", 0},       {"url", 0},              {"query", 0},
    {"skill", 0},         {"prompt", 0},           {"description", 0},
    {"message", 0},
};

const char *view_tool_arg_value(const cJSON *input)
{
    for (int i = 0; i < COUNT(TOOL_ARG_KEYS); i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(input, TOOL_ARG_KEYS[i].key));
        if (v && *v)
            return v;
    }
    return NULL;
}

static const char *shorten_path(const char *cwd, const char *value, char *scratch,
                                size_t size)
{
    if (value[0] != '/')
        return value;
    if (cwd) {
        size_t n = strlen(cwd);
        if (strncmp(value, cwd, n) == 0 && value[n] == '/')
            return value + n + 1;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t n = strlen(home);
        if (strncmp(value, home, n) == 0 && value[n] == '/') {
            snprintf(scratch, size, "~/%s", value + n + 1);
            return scratch;
        }
    }
    return value;
}

/* Memory mode's own tools: plain names from core, mcp__optchat__ ones from
 * the CLIs. Their arguments are numbers, which the keys above never match. */
static const char *memory_tool(const char *name)
{
    if (name && !strncmp(name, "mcp__optchat__", 14))
        return name + 14;
    return name;
}

static int memory_tool_argument(const char *name, const cJSON *input, char *out, size_t size)
{
    name = memory_tool(name);
    if (!name)
        return 0;
    const cJSON *id = cJSON_GetObjectItem(input, "id"), *n = cJSON_GetObjectItem(input, "n");
    if (!strcmp(name, "zoom") && cJSON_IsNumber(id) && cJSON_IsNumber(n)) {
        snprintf(out, size, "%ld+%ld", (long)id->valuedouble, (long)n->valuedouble);
        return 1;
    }
    if (!strcmp(name, "date") && cJSON_IsNumber(id)) {
        snprintf(out, size, "%ld", (long)id->valuedouble);
        return 1;
    }
    const char *task = cJSON_GetStringValue(cJSON_GetObjectItem(input, "task"));
    if (!strcmp(name, "agent") && task && *task) {
        text_block(task, out, size);
        return 1;
    }
    return 0;
}

const char *view_tool_value(const char *name, const cJSON *input, char *scratch, size_t size)
{
    if (memory_tool_argument(name, input, scratch, size))
        return scratch;
    return view_tool_arg_value(input);
}

void view_tool_argument(const backend_event *ev, const char *cwd, char *out, size_t size)
{
    char arg[4096] = "";

    if (ev->input_json) {
        cJSON *input = cJSON_Parse(ev->input_json);
        if (input && memory_tool_argument(ev->name, input, arg, sizeof arg)) {
            cJSON_Delete(input);
        } else if (input) {
            const char *v = view_tool_arg_value(input);
            if (v) {
                char scratch[1024];
                text_block(shorten_path(cwd, v, scratch, sizeof scratch), arg, sizeof arg);
            }
            cJSON_Delete(input);
        }
    } else if (ev->arg) {
        text_block(ev->arg, arg, sizeof arg);
    }
    snprintf(out, size, "%s", arg);
}

int view_policy_denial(const char *text, char *out, size_t size)
{
    static const char HEAD[] = "Permission to use ", TAIL[] = " has been denied.";
    if (!text || strncmp(text, HEAD, sizeof HEAD - 1))
        return 0;
    const char *p = text + sizeof HEAD - 1;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\n' || p[n - 1] == ' '))
        n--;
    if (n < sizeof TAIL - 1 || strncmp(p + n - (sizeof TAIL - 1), TAIL, sizeof TAIL - 1))
        return 0;
    n -= sizeof TAIL - 1;
    /* "<Tool>" or "<Tool> with <field> <value>" */
    const char *with = memmem(p, n, " with ", 6);
    const char *v = "";
    size_t vn = 0;
    if (with) {
        const char *field = with + 6, *sp = memchr(field, ' ', (size_t)(p + n - field));
        if (sp) {
            v = sp + 1;
            vn = (size_t)(p + n - v);
        }
    }
    if (size)
        snprintf(out, size, "%.*s", (int)vn, v);
    return 1;
}

int view_tool_path(const char *input_json, const char *cwd, char *out, size_t size)
{
    if (!input_json)
        return 0;

    cJSON *input = cJSON_Parse(input_json);
    if (!input)
        return 0;

    const char *found = NULL;
    for (int i = 0; i < COUNT(TOOL_ARG_KEYS) && !found; i++) {
        if (!TOOL_ARG_KEYS[i].is_path)
            continue;
        const char *v =
            cJSON_GetStringValue(cJSON_GetObjectItem(input, TOOL_ARG_KEYS[i].key));
        if (v && *v)
            found = v;
    }
    if (found) {
        if (found[0] == '/')
            snprintf(out, size, "%s", found);
        else
            snprintf(out, size, "%s/%s", cwd ? cwd : ".", found);
    }
    cJSON_Delete(input);
    return found != NULL;
}

static void view_activity(const char *marker, const char *text, enum ui_role role)
{
    int indent = TOOL_INDENT + nest + (int)ui_cells(marker) + 1;
    int columns = ui_columns();
    int budget = columns - indent;
    if (budget < 8)
        budget = 8;

    nest_pad(TOOL_INDENT);
    if (*marker) {
        ui_esc(ui_style(UI_CHROME));
        ui_put(marker);
        ui_esc(ui_style(UI_RESET));
        ui_put(" ");
    }

    if (!text || !*text) {
        ui_put("\n");
        return;
    }

    struct ui_wrap w = {0};
    w.budget = (size_t)budget;
    w.indent = indent;
    w.role = role;
    ui_wrap_paint(text, &w);
}

static void tool_tag(const char *name, int background, char *out, size_t size)
{
    size_t t = 0;
    size_t mark = background ? 4 : 0;
    out[t++] = '[';
    for (const char *p = memory_tool(name); *p && t + 2 + mark < size; p++)
        out[t++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
    if (background) {
        memcpy(out + t, " \xe2\x86\x97", 4);
        t += 4;
    }
    out[t++] = ']';
    out[t] = '\0';
}

#define TOOL_CALL_ROWS 24

static unsigned char *shell_spans(const char *name, const char *text, size_t len)
{
    if (!toolstyle_is_shell(name) || !len)
        return NULL;

    unsigned char *spans = malloc(len);
    if (spans)
        highlight_shell(text, len, spans);
    return spans;
}

void view_tool_call(const char *name, const char *arg) { tool_call(name, arg, 0); }

static void tool_call(const char *name, const char *arg, int background)
{
    char tag[64];
    tool_tag(name, background, tag, sizeof tag);

    int indent = TOOL_INDENT + nest + (int)ui_cells(tag) + 1;
    int columns = ui_columns();

    nest_pad(TOOL_INDENT);
    ui_esc(ui_style(UI_TOOL));
    ui_put(tag);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");

    if (!arg || !*arg) {
        ui_put("\n");
        return;
    }
    int budget = columns - indent;
    if (budget < 8)
        budget = 8;

    unsigned char *spans = shell_spans(name, arg, strlen(arg));

    struct ui_wrap w = {0};
    w.budget = (size_t)budget;
    w.indent = indent;
    w.role = UI_RESET;
    w.max_rows = TOOL_CALL_ROWS + 1;
    w.measure = 1;
    int folds = ui_wrap_paint(arg, &w) > TOOL_CALL_ROWS;
    w.measure = 0;
    w.max_rows = viewport_fold_open() ? 0 : TOOL_CALL_ROWS;
    w.spans = spans;
    ui_wrap_paint(arg, &w);
    free(spans);
    if (folds)
        viewport_fold_enable();
}

static unsigned char *row_spans(const char *name, const char *row, size_t prefix)
{
    size_t len = strlen(row);
    if (!toolstyle_is_shell(name) || len <= prefix)
        return NULL;

    unsigned char *spans = malloc(len);
    if (!spans)
        return NULL;
    memset(spans, (unsigned char)UI_RESET, prefix);
    highlight_shell(row + prefix, len - prefix, spans + prefix);
    return spans;
}

static int cluster_budget(void)
{
    int budget = ui_columns() - TOOL_INDENT - nest - 2;
    return budget < 8 ? 8 : budget;
}

static void cluster_paint(const char *line, const unsigned char *spans)
{
    size_t tag = strcspn(line, "]");
    if (line[tag])
        tag++;

    size_t len = strlen(line);
    size_t fit = ui_fit_visible(line, len, (size_t)cluster_budget());
    if (fit < tag)
        fit = tag;
    if (fit > len)
        fit = len;

    nest_pad(TOOL_INDENT);
    ui_esc(ui_style(UI_TOOL));
    ui_putn(line, tag);
    ui_esc(ui_style(UI_RESET));
    if (spans)
        ui_put_spans(line + tag, fit - tag, spans + tag, UI_RESET);
    else
        ui_putn(line + tag, fit - tag);
    if (fit < len)
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

#define TOOL_PREVIEW_ROWS 3

#define PREVIEW_INDENT 4

static int preview_budget(void)
{
    int budget = ui_columns() - PREVIEW_INDENT - nest - 2;
    return budget < 8 ? 8 : budget;
}

static int preview_line(const char *start, size_t n, int budget, enum ui_role role)
{
    char line[1024];
    if (n >= sizeof line)
        n = sizeof line - 1;
    memcpy(line, start, n);
    line[n] = '\0';

    char clipped[1024];
    text_one_line(line, clipped, sizeof clipped);
    if (!*clipped)
        return 0;

    size_t skip = 0;
    size_t fit = ui_wrap_row(clipped, strlen(clipped), (size_t)budget, &skip, NULL);
    nest_pad(PREVIEW_INDENT);
    ui_esc(ui_style(role));
    ui_putn(clipped, fit);
    if (clipped[fit])
        ui_put("…");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    return 1;
}

static void preview_elision(int lines)
{
    if (lines <= 0)
        return;
    nest_pad(PREVIEW_INDENT);
    ui_esc(ui_style(UI_DIM));
    ui_printf("+%d line%s", lines, lines == 1 ? "" : "s");
    ui_esc(ui_style(UI_RESET));
    viewport_fold_enable();
    ui_put("\n");
}

static int preview_open(const char *text, enum ui_role role)
{
    if (!viewport_fold_open())
        return 0;
    struct ui_wrap w = {0};
    w.budget = (size_t)preview_budget();
    w.first_indent = w.indent = PREVIEW_INDENT + nest;
    w.role = role;
    ui_wrap_paint(text, &w);
    viewport_fold_enable();
    return 1;
}

void view_tool_error(const char *text)
{
    if (!text || !*text || preview_open(text, UI_ERROR))
        return;

    const char *head = NULL;
    size_t      head_n = 0;
    const char *tail[TOOL_PREVIEW_ROWS] = {0};
    size_t      tail_n[TOOL_PREVIEW_ROWS] = {0};
    int         total = 0;

    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t      n = nl ? (size_t)(nl - p) : strlen(p);
        size_t      i = 0;
        while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r'))
            i++;
        if (i < n) {
            if (!total++) {
                head = p;
                head_n = n;
            } else {
                int slot = (total - 2) % TOOL_PREVIEW_ROWS;
                tail[slot] = p;
                tail_n[slot] = n;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!total)
        return;

    int budget = preview_budget();
    preview_line(head, head_n, budget, UI_ERROR);

    int kept = total - 1 < TOOL_PREVIEW_ROWS ? total - 1 : TOOL_PREVIEW_ROWS;
    preview_elision(total - 1 - kept);
    for (int i = 0; i < kept; i++) {
        int slot = (total - 1 - kept + i) % TOOL_PREVIEW_ROWS;
        preview_line(tail[slot], tail_n[slot], budget, UI_ERROR);
    }
}

static void view_tool_output(const char *text, enum ui_role role)
{
    if (!text || !*text || preview_open(text, role))
        return;

    int columns = ui_columns();
    int budget = columns - PREVIEW_INDENT - nest - 2;
    if (budget < 8)
        budget = 8;

    const char *p = text;
    int shown = 0;
    while (*p && shown < TOOL_PREVIEW_ROWS) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';

        char clipped[1024];
        text_one_line(line, clipped, sizeof clipped);
        if (*clipped) {
            size_t skip = 0;
            size_t fit = ui_wrap_row(clipped, strlen(clipped), (size_t)budget, &skip, NULL);
            nest_pad(PREVIEW_INDENT);
            ui_esc(ui_style(role));
            ui_putn(clipped, fit);
            if (clipped[fit])
                ui_put("…");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            shown++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }

    int remaining = 0;
    for (const char *q = p; *q; q++)
        if (*q == '\n' && q[1])
            remaining++;
    if (*p && shown >= TOOL_PREVIEW_ROWS)
        remaining++;
    preview_elision(remaining);
}
