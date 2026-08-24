#include "boardcfgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ask.h"
#include "boardcfg.h"
#include "edit.h"
#include "form.h"
#include "pick.h"
#include "vendor/agents/backend.h"

#define CFG_HINT "enter edit  \xc2\xb7  d delete a kind  \xc2\xb7  esc done"

enum row_kind {
    ROW_HEAD,
    ROW_COUNT,
    ROW_TOGGLE,
    ROW_PROFILE,
    ROW_SERVING,
    ROW_BACKEND,
    ROW_PROMPT,
    ROW_VERIFY,
    ROW_KIND,
    ROW_KIND_NEW,
};

struct row {
    enum row_kind kind;
    const char   *label;
    const char   *about;

    int *count;
    int  low, high;
    const char *units;

    int            role_at;
    int            kind_at;
    int            backend_at;
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

    head(rows, n, "audit");
    count_row(rows, n, "file threshold", &c->audit_files, 0, 500, NULL);
    count_row(rows, n, "line threshold", &c->audit_lines, 0, 100000, NULL);

    head(rows, n, "sweep");
    count_row(rows, n, "interval", &c->sweep_every, 0, 500, "cards");

    head(rows, n, "kinds");
    for (int i = 0; i < c->kinds_n && *n < ROWS_MAX - 2; i++) {
        rows[*n].kind = ROW_KIND;
        rows[*n].label = c->kinds[i].name;
        rows[*n].kind_at = i;
        (*n)++;
    }
    if (c->kinds_n < BOARD_KINDS_MAX && *n < ROWS_MAX - 1) {
        rows[*n].kind = ROW_KIND_NEW;
        rows[*n].label = "add";
        (*n)++;
    }

    head(rows, n, "archive");
    count_row(rows, n, "after", &c->archive_after, 0, 3650, "days");

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

    head(rows, n, "models");
    for (int i = 0; i < c->roles_n && *n < ROWS_MAX - 1; i++) {
        rows[*n].kind = ROW_PROFILE;
        rows[*n].label = c->roles[i].name;
        rows[*n].role_at = i;
        (*n)++;
    }

    int stands = 0;
    for (int i = 0; i < c->roles_n; i++)
        stands += c->roles[i].step[0] != '\0';
    if (stands) {
        head(rows, n, "skippable");
        for (int i = 0; i < c->roles_n && *n < ROWS_MAX - 1; i++)
            if (c->roles[i].step[0])
                toggle_row(rows, n, c->roles[i].name, &c->roles[i].skippable);
    }

    head(rows, n, "prompts");
    for (int i = 0; i < c->roles_n && *n < ROWS_MAX - 1; i++) {
        rows[*n].kind = ROW_PROMPT;
        rows[*n].label = c->roles[i].name;
        rows[*n].role_at = i;
        (*n)++;
    }

    head(rows, n, "merge");
    rows[*n].kind = ROW_VERIFY;
    rows[*n].label = "check";
    (*n)++;

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
    case ROW_PROFILE: {
        const struct board_profile *p = &c->roles[r->role_at];
        enum board_tier             tier = boardcfg_tier_or_med(p->tier);
        const struct board_backend *b = boardcfg_backend(c, c->serving);
        const char                 *model = b ? b->level[tier].model : "";
        snprintf(out, size, "%s · %s · %s", boardcfg_tier_name(tier), c->serving,
                 model[0] ? model : "default");
        break;
    }
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
    case ROW_PROMPT: {
        const char *text = c->roles[r->role_at].prompt;
        int         lines = 1;
        for (const char *s = text; s && *s; s++)
            lines += *s == '\n';
        snprintf(out, size, "%d line%s", lines, lines == 1 ? "" : "s");
        break;
    }
    case ROW_VERIFY:
        snprintf(out, size, "%s", c->verify[0] ? c->verify : "none");
        break;
    case ROW_KIND: {
        const struct board_kind *k = &c->kinds[r->kind_at];
        size_t                   at = (size_t)snprintf(out, size, "p%d", k->priority);
        for (int i = 0; i < BOARD_STEPS && at < size; i++)
            if (k->steps & (1u << i))
                at += (size_t)snprintf(out + at, size - at, " \xc2\xb7 %s",
                                       boardcfg_step_name((enum board_step)i));
        if (!k->steps && at < size)
            snprintf(out + at, size - at, " \xc2\xb7 nothing after the worker");
        break;
    }
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

static const char *const TIERS[] = {"low", "med", "high"};

static void edit_profile(struct board_cfg *c, int role_at)
{
    struct board_profile *p = &c->roles[role_at];

    struct pick_item items[BOARD_TIERS];
    int              at = 0;
    for (int t = 0; t < BOARD_TIERS; t++) {
        const struct board_backend *b = boardcfg_backend(c, c->serving);
        const char                 *model = b ? b->level[t].model : "";
        items[t] = (struct pick_item){TIERS[t], model[0] ? model : "default"};
        if (!strcmp(TIERS[t], p->tier))
            at = t;
    }

    char title[128];
    snprintf(title, sizeof title, "%s tier", p->name);

    int chosen = pick_run(title, items, BOARD_TIERS, at);
    if (chosen >= 0)
        snprintf(p->tier, sizeof p->tier, "%s", TIERS[chosen]);
}

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
                                        sizeof model[t], NULL, 0};
        n++;
        snprintf(labels[n], sizeof labels[n], "%s effort", name);
        fields[n] = (struct form_field){labels[n], FORM_CHOICE, effort[t],
                                        sizeof effort[t], EFFORTS, COUNT(EFFORTS)};
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

static void edit_prompt(struct board_cfg *c, int at)
{
    char *text = edit_run(c->roles[at].prompt, ".md");
    if (!text)
        return;
    free(c->roles[at].prompt);
    c->roles[at].prompt = text;
}

static void edit_verify(struct board_cfg *c)
{
    char *said = ask_run("command a card must pass to land", c->verify);
    if (!said)
        return;
    snprintf(c->verify, sizeof c->verify, "%s", said);
    free(said);
}


static const char *const YES_NO[] = {"no", "yes"};
static const char *const LEVELS[] = {"0", "1", "2", "3"};

static void edit_kind(struct board_cfg *c, int at)
{
    struct board_kind *k = &c->kinds[at];

    char name[32], means[256], priority[8], approval[512];
    char steps[BOARD_STEPS][8];
    char *prompt = calloc(1, 8192);
    if (!prompt)
        return;

    snprintf(name, sizeof name, "%s", k->name);
    snprintf(means, sizeof means, "%s", k->means ? k->means : "");
    snprintf(priority, sizeof priority, "%d", k->priority);
    snprintf(prompt, 8192, "%s", k->prompt ? k->prompt : "");
    snprintf(approval, sizeof approval, "%s", k->approval_prompt ? k->approval_prompt : "");
    for (int i = 0; i < BOARD_STEPS; i++)
        snprintf(steps[i], sizeof steps[i], "%s",
                 k->steps & (1u << i) ? "yes" : "no");

    struct form_field fields[4 + BOARD_STEPS + 1];
    int               fields_n = 0;
    fields[fields_n++] = (struct form_field){"name", FORM_TEXT, name, sizeof name, NULL, 0};
    fields[fields_n++] = (struct form_field){"means", FORM_TEXT, means, sizeof means, NULL, 0};
    fields[fields_n++] = (struct form_field){"priority", FORM_CHOICE, priority,
                                             sizeof priority, LEVELS, 4};
    for (int i = 0; i < BOARD_STEPS; i++)
        fields[fields_n++] = (struct form_field){
            boardcfg_step_name((enum board_step)i), FORM_CHOICE, steps[i],
            sizeof steps[i], YES_NO, 2};
    fields[fields_n++] = (struct form_field){"approval prompt", FORM_TEXT, approval,
                                             sizeof approval, NULL, 0};
    fields[fields_n++] = (struct form_field){"prompt", FORM_TEXT, prompt, 8192, NULL, 0};

    static const char *const NOTES[] = {
        "means is what the classifier is told this kind is.",
        "prompt is what a worker given one is told, before the card.",
        "the steps a card of this kind takes once a worker has had it.",
        "none of them: the worker writes it and the card is done.",
        "approval prompt is what a worker is told when you approve one in",
        "review; empty finishes the card there instead.",
    };

    struct form f = {.title = "kind", .notes = NOTES, .notes_n = 6,
                     .fields = fields, .fields_n = fields_n};
    if (!form_run(&f) || !name[0]) {
        free(prompt);
        return;
    }

    snprintf(k->name, sizeof k->name, "%s", name);
    k->priority = atoi(priority);
    k->steps = 0;
    for (int i = 0; i < BOARD_STEPS; i++)
        if (!strcmp(steps[i], "yes"))
            k->steps |= 1u << i;

    char *kept_means = strdup(means);
    if (kept_means) {
        free(k->means);
        k->means = kept_means;
    }
    char *kept_approval = strdup(approval);
    if (kept_approval) {
        free(k->approval_prompt);
        k->approval_prompt = kept_approval;
    }
    free(k->prompt);
    k->prompt = prompt;
}

static void add_kind(struct board_cfg *c)
{
    if (c->kinds_n >= BOARD_KINDS_MAX)
        return;
    struct board_kind *k = &c->kinds[c->kinds_n];
    memset(k, 0, sizeof *k);
    k->steps = (1u << BOARD_STEPS) - 1u;
    k->means = strdup("");
    k->prompt = strdup("");
    c->kinds_n++;

    edit_kind(c, c->kinds_n - 1);

    if (!c->kinds[c->kinds_n - 1].name[0]) {
        free(c->kinds[c->kinds_n - 1].means);
        free(c->kinds[c->kinds_n - 1].prompt);
        free(c->kinds[c->kinds_n - 1].approval_prompt);
        c->kinds_n--;
    }
}

static void drop_kind(struct board_cfg *c, int at)
{
    if (at < 0 || at >= c->kinds_n)
        return;
    free(c->kinds[at].means);
    free(c->kinds[at].prompt);
    free(c->kinds[at].approval_prompt);
    for (int i = at; i + 1 < c->kinds_n; i++)
        c->kinds[i] = c->kinds[i + 1];
    c->kinds_n--;
    memset(&c->kinds[c->kinds_n], 0, sizeof c->kinds[c->kinds_n]);
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
        int              pressed = 0;
        at = pick_run_live("board config", items, n, at, &live, PICK_SEARCH_SLASH,
                           "d", &pressed);
        if (at < 0)
            break;

        if (pressed == 'd') {
            if (rows[at].kind == ROW_KIND) {
                drop_kind(c, rows[at].kind_at);
                touched = 1;
            }
            continue;
        }

        switch (rows[at].kind) {
        case ROW_COUNT:   edit_count(&rows[at]); touched = 1; break;
        case ROW_TOGGLE:  *rows[at].count = !*rows[at].count; touched = 1; break;
        case ROW_PROFILE: edit_profile(c, rows[at].role_at); touched = 1; break;
        case ROW_SERVING: edit_serving(c); touched = 1; break;
        case ROW_BACKEND: edit_backend(c, rows[at].backend_at); touched = 1; break;
        case ROW_PROMPT:  edit_prompt(c, rows[at].role_at); touched = 1; break;
        case ROW_VERIFY:  edit_verify(c); touched = 1; break;
        case ROW_KIND:    edit_kind(c, rows[at].kind_at); touched = 1; break;
        case ROW_KIND_NEW: add_kind(c); touched = 1; break;
        default:          break;
        }
    }

    if (touched)
        boardcfg_set(c);
    boardcfg_free(c);
}
