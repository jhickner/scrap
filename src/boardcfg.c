#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "mdcfg.h"
#include "boarddefaults.h"
#include "sessionfork.h"
#include "vendor/agents/backend.h"

static const char *const TIER_NAMES[BOARD_TIERS] = {"low", "med", "high"};

const char *boardcfg_tier_name(enum board_tier tier)
{
    if (tier < 0 || tier >= BOARD_TIERS)
        return "";
    return TIER_NAMES[tier];
}

enum board_tier boardcfg_tier_from_name(const char *name)
{
    if (name)
        for (int i = 0; i < BOARD_TIERS; i++)
            if (!strcmp(name, TIER_NAMES[i]))
                return (enum board_tier)i;
    return BOARD_TIERS;
}

static enum board_tier tier_or_med(const char *name)
{
    enum board_tier tier = boardcfg_tier_from_name(name);
    return tier < BOARD_TIERS ? tier : BOARD_TIER_MED;
}

int boardcfg_backend_choices(const struct board_cfg *c, const char **out,
                             int max)
{
    int n = 0;
    if (n < max)
        out[n++] = "";
    for (int i = 0; i < c->backends_n && n < max; i++)
        out[n++] = c->backends[i].name;
    return n;
}

int boardcfg_tier_choices(const char **out, int max)
{
    int n = 0;
    if (n < max)
        out[n++] = "";
    for (int t = 0; t < BOARD_TIERS && n < max; t++)
        out[n++] = boardcfg_tier_name((enum board_tier)t);
    return n;
}

static const struct board_backend *backend_of(const struct board_cfg *c,
                                              const char *name)
{
    if (!c || !name || !*name)
        return NULL;
    for (int i = 0; i < c->backends_n; i++)
        if (!strcmp(c->backends[i].name, name))
            return &c->backends[i];
    return NULL;
}

static const char *const IN_NAMES[BOARD_INS] = {"worktree", "repo"};

static enum board_in in_from_name(const char *name)
{
    if (name)
        for (int i = 0; i < BOARD_INS; i++)
            if (!strcmp(name, IN_NAMES[i]))
                return (enum board_in)i;
    return BOARD_IN_WORKTREE;
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void backend_defaults(struct board_backend *b, const char *name)
{
    memset(b, 0, sizeof *b);
    snprintf(b->name, sizeof b->name, "%s", name);
    snprintf(b->level[BOARD_TIER_LOW].effort, sizeof b->level[0].effort, "low");
    snprintf(b->level[BOARD_TIER_MED].effort, sizeof b->level[0].effort, "medium");
    snprintf(b->level[BOARD_TIER_HIGH].effort, sizeof b->level[0].effort, "high");

    if (!strcmp(name, "claude")) {
        snprintf(b->level[BOARD_TIER_LOW].model, sizeof b->level[0].model, "haiku");
        snprintf(b->level[BOARD_TIER_MED].model, sizeof b->level[0].model, "opus[1m]");
        snprintf(b->level[BOARD_TIER_HIGH].model, sizeof b->level[0].model, "opus[1m]");
    } else if (!strcmp(name, "codex")) {
        snprintf(b->level[BOARD_TIER_LOW].model, sizeof b->level[0].model, "gpt-5.6-terra");
        snprintf(b->level[BOARD_TIER_MED].model, sizeof b->level[0].model, "gpt-5.6-sol");
        snprintf(b->level[BOARD_TIER_HIGH].model, sizeof b->level[0].model, "gpt-5.6-sol");
    } else if (!strcmp(name, "pi")) {
        snprintf(b->level[BOARD_TIER_LOW].model, sizeof b->level[0].model,
                 "openrouter/moonshotai/kimi-k3");
        snprintf(b->level[BOARD_TIER_MED].model, sizeof b->level[0].model,
                 "openrouter/openai/gpt-5.6-terra");
        snprintf(b->level[BOARD_TIER_HIGH].model, sizeof b->level[0].model,
                 "openrouter/openai/gpt-5.6-sol");
        snprintf(b->level[BOARD_TIER_MED].effort, sizeof b->level[0].effort, "medium");
        snprintf(b->level[BOARD_TIER_HIGH].effort, sizeof b->level[0].effort, "medium");
    }
}

static void defaults(struct board_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->workers = 3;
    c->archive_after = 14;
    c->closed_shown = 20;
    c->open_shown = 20;
    snprintf(c->view, sizeof c->view, "list");

    snprintf(c->serving, sizeof c->serving, "claude");
    for (const char *const *b = backend_names(); *b && c->backends_n < BOARD_BACKENDS_MAX; b++)
        backend_defaults(&c->backends[c->backends_n++], *b);
}

#define BOARD_DIR "board"

static int board_path(char *out, size_t size, const char *leaf, const char *name)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, leaf))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s.md", dir, name) < size;
}

#define STALE_MAX 64

/* Actions used to be copied into the config as markdown. They are
 * compiled in now, so the copies are stale the moment the binary changes. */
static void drop_stale(const char *leaf)
{
    char base[4096];
    if (!path_config_dir(base, sizeof base))
        return;

    char dir[4200];
    if ((size_t)snprintf(dir, sizeof dir, "%s/%s", base, leaf) >= sizeof dir)
        return;

    char names[STALE_MAX][MDCFG_NAME];
    int  n = mdcfg_list(dir, names, STALE_MAX);
    for (int i = 0; i < n; i++) {
        char path[4400];
        if ((size_t)snprintf(path, sizeof path, "%s/%s.md", dir, names[i]) <
            sizeof path)
            unlink(path);
    }
    rmdir(dir);
}

static const struct board_default *defs = board_defaults;
static int                         defs_n;

void boardcfg_defaults(const struct board_default *table, int n)
{
    defs = table ? table : board_defaults;
    defs_n = table ? n : board_defaults_n;
    boardcfg_reload();
}

/* The name a compiled-in file stands under: actions/plan.md is the plan action. */
static const char *default_name(const char *path, const char *leaf, char *out,
                                size_t size)
{
    size_t at = strlen(leaf);
    if (strncmp(path, leaf, at) || path[at] != '/')
        return NULL;

    const char *file = path + at + 1;
    const char *dot = strrchr(file, '.');
    size_t      len = dot ? (size_t)(dot - file) : strlen(file);
    if (!len || len >= size)
        return NULL;
    memcpy(out, file, len);
    out[len] = '\0';
    return out;
}

/* "implement, merge" -> the names it lists, trimmed */
static int names_of(const char *list, char out[][BOARD_STEP_NAME], int max)
{
    char *copy = strdup(list ? list : "");
    if (!copy)
        return 0;

    const char *parts[BOARD_PIPELINE_LONG];
    int         cap = (int)(sizeof parts / sizeof *parts);
    int         n = text_split_commas(copy, parts, max < cap ? max : cap);
    for (int i = 0; i < n; i++)
        snprintf(out[i], BOARD_STEP_NAME, "%s", parts[i]);

    free(copy);
    return n;
}

static void read_settings(struct board_cfg *c)
{
    char path[4300];
    if (!board_path(path, sizeof path, BOARD_DIR, "settings"))
        return;

    struct mdcfg m;
    if (!mdcfg_load(&m, path))
        return;

    c->workers = mdcfg_int(&m, "workers", c->workers);
    c->auto_pull = mdcfg_int(&m, "auto pull", c->auto_pull);
    c->auto_pick = mdcfg_int(&m, "auto pick", c->auto_pick);
    c->archive_after = mdcfg_int(&m, "archive after", c->archive_after);
    /* the lane was called done before it was called closed */
    int shown = mdcfg_int(&m, "done shown", c->closed_shown);
    c->closed_shown = mdcfg_int(&m, "closed shown", shown);
    /* the lane was called the backlog before the columns went */
    c->open_shown = mdcfg_int(&m, "open shown",
                              mdcfg_int(&m, "backlog shown", c->open_shown));

    const char *serving = mdcfg_get(&m, "serving");
    if (*serving)
        snprintf(c->serving, sizeof c->serving, "%s", serving);

    const char *view = mdcfg_get(&m, "view");
    if (*view)
        snprintf(c->view, sizeof c->view, "%s", view);

    mdcfg_free(&m);
}

static void read_actions(struct board_cfg *c)
{
    for (int i = 0; i < defs_n && c->actions_n < BOARD_ACTIONS_MAX; i++) {
        char name[MDCFG_NAME];
        if (!default_name(defs[i].path, "actions", name, sizeof name))
            continue;

        struct mdcfg m;
        if (!mdcfg_parse(&m, strdup(defs[i].text)))
            continue;

        struct board_action *p = &c->actions[c->actions_n++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", name);
        snprintf(p->fail_marker, sizeof p->fail_marker, "%s",
                 mdcfg_get(&m, "fail marker"));
        snprintf(p->tier, sizeof p->tier, "%s", mdcfg_get(&m, "tier"));
        if (!p->tier[0])
            snprintf(p->tier, sizeof p->tier, "%s",
                     boardcfg_tier_name(BOARD_TIER_MED));

        p->where = in_from_name(mdcfg_get(&m, "in"));
        p->on_capture = !strcmp(mdcfg_get(&m, "on"), "capture");
        p->commits = !strcmp(mdcfg_get(&m, "commits"), "yes");
        p->closes = !strcmp(mdcfg_get(&m, "closes"), "yes");
        p->needs_n = names_of(mdcfg_get(&m, "needs"), p->needs, BOARD_NEEDS);

        p->prompt = dup_or_null(m.body ? m.body : "");
        mdcfg_free(&m);
    }
}

static void level_key(char *out, size_t size, enum board_tier tier,
                      const char *what)
{
    snprintf(out, size, "%s %s", boardcfg_tier_name(tier), what);
}

static void read_backends(struct board_cfg *c)
{
    for (int i = 0; i < c->backends_n; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/backends", c->backends[i].name))
            continue;

        struct mdcfg m;
        if (!mdcfg_load(&m, path))
            continue;

        for (int t = 0; t < BOARD_TIERS; t++) {
            struct board_level *l = &c->backends[i].level[t];
            char                key[64];
            level_key(key, sizeof key, (enum board_tier)t, "model");
            snprintf(l->model, sizeof l->model, "%s", mdcfg_get(&m, key));
            level_key(key, sizeof key, (enum board_tier)t, "effort");
            snprintf(l->effort, sizeof l->effort, "%s", mdcfg_get(&m, key));
        }
        mdcfg_free(&m);
    }
}

static void read_pipelines(struct board_cfg *c)
{
    for (int i = 0; i < defs_n && c->pipelines_n < BOARD_PIPELINES_MAX; i++) {
        char name[MDCFG_NAME];
        if (!default_name(defs[i].path, "pipelines", name, sizeof name))
            continue;

        struct mdcfg m;
        if (!mdcfg_parse(&m, strdup(defs[i].text)))
            continue;

        struct board_pipeline *p = &c->pipelines[c->pipelines_n++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", name);
        p->actions_n = names_of(mdcfg_get(&m, "actions"), p->actions,
                                BOARD_PIPELINE_LONG);
        mdcfg_free(&m);
    }
}

static int write_settings(const struct board_cfg *c)
{
    char path[4300];
    if (!board_path(path, sizeof path, BOARD_DIR, "settings"))
        return 0;

    char nums[6][32];
    snprintf(nums[0], sizeof nums[0], "%d", c->workers);
    snprintf(nums[1], sizeof nums[1], "%d", c->auto_pull);
    snprintf(nums[2], sizeof nums[2], "%d", c->auto_pick);
    snprintf(nums[3], sizeof nums[3], "%d", c->archive_after);
    snprintf(nums[4], sizeof nums[4], "%d", c->closed_shown);
    snprintf(nums[5], sizeof nums[5], "%d", c->open_shown);

    const char *keys[] = {"serving", "view", "workers", "auto pull",
                          "auto pick", "archive after",
                          "closed shown", "open shown"};
    const char *vals[] = {c->serving, c->view, nums[0], nums[1], nums[2],
                          nums[3], nums[4], nums[5]};

    return mdcfg_write(path, keys, vals, 8,
        "serving is the backend every tiered action runs on.\n"
        "view is list or grid: the board as rows, or as tiles in lanes.\n"
        "workers is how many may run at once.\n"
        "auto pull is 1 to start queued cards on a free worker, 0 to wait to\n"
        "be told.\n"
        "auto pick is 1 to hand a worker the next queued card when its current\n"
        "task completes, switching backends to match the card; 0 to leave it on\n"
        "the card until you take it.\n"
        "archive after is days a closed card stays on the board; zero forever.\n"
        "closed shown and open shown are how many cards those lanes list; zero\n"
        "lists them all.\n"
        "\n"
        "The actions a card can run, and the pipelines that name a run of them,\n"
        "are built into the binary and are not read from here.\n");
}

static int write_backends(const struct board_cfg *c)
{
    int ok = 1;
    for (int i = 0; i < c->backends_n; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/backends", c->backends[i].name)) {
            ok = 0;
            continue;
        }

        char        keys[BOARD_TIERS * 2][64];
        const char *k[BOARD_TIERS * 2], *v[BOARD_TIERS * 2];
        int         n = 0;
        for (int t = 0; t < BOARD_TIERS; t++) {
            level_key(keys[n], sizeof keys[n], (enum board_tier)t, "model");
            k[n] = keys[n];
            v[n++] = c->backends[i].level[t].model;
            level_key(keys[n], sizeof keys[n], (enum board_tier)t, "effort");
            k[n] = keys[n];
            v[n++] = c->backends[i].level[t].effort;
        }
        if (!mdcfg_write(path, k, v, n,
                         "what low, med and high mean on this backend.\n"
                         "empty is the backend's own default.\n"))
            ok = 0;
    }
    return ok;
}

static int write_out(const struct board_cfg *c)
{
    int ok = write_settings(c);
    if (!write_backends(c))
        ok = 0;
    return ok;
}

static struct board_cfg  cache;
static int               loaded;
static struct board_action serving_actions[BOARD_ACTIONS_MAX];

static void resolve(void)
{
    const struct board_backend *b = backend_of(&cache, cache.serving);

    for (int i = 0; i < cache.actions_n; i++) {
        serving_actions[i] = cache.actions[i];

        enum board_tier tier = tier_or_med(cache.actions[i].tier);

        snprintf(serving_actions[i].backend, sizeof serving_actions[i].backend, "%s",
                 cache.serving[0] ? cache.serving : "claude");
        snprintf(serving_actions[i].model, sizeof serving_actions[i].model, "%s",
                 b ? b->level[tier].model : "");
        snprintf(serving_actions[i].effort, sizeof serving_actions[i].effort, "%s",
                 b ? b->level[tier].effort : "");
    }
}

static void cache_free(void)
{
    for (int i = 0; i < cache.actions_n; i++)
        free(cache.actions[i].prompt);
}

static void read_all(void)
{
    if (defs == board_defaults)
        defs_n = board_defaults_n;

    cache_free();
    defaults(&cache);
    drop_stale(BOARD_DIR "/roles");
    drop_stale(BOARD_DIR "/actions");
    drop_stale(BOARD_DIR "/kinds");
    drop_stale(BOARD_DIR "/pipelines");

    read_settings(&cache);
    read_actions(&cache);
    read_backends(&cache);
    read_pipelines(&cache);

    char seed[4300];
    if (board_path(seed, sizeof seed, BOARD_DIR, "settings") &&
        access(seed, F_OK))
        write_out(&cache);
}

static void load(void)
{
    if (loaded)
        return;
    loaded = 1;
    read_all();
    resolve();
}

void boardcfg_reload(void)
{
    loaded = 1;
    read_all();
    resolve();
}

const struct board_cfg *boardcfg(void)
{
    load();
    return &cache;
}

/* The actions and pipelines are compiled in, so anything wrong here is wrong
 * in board/ in the source tree, not in the config. */
int boardcfg_missing(char *out, size_t size)
{
    load();

    if (!cache.actions_n) {
        snprintf(out, size, "no actions are built into this binary");
        return 1;
    }

    char   who[128] = "";
    size_t at = 0;
    for (int i = 0; i < cache.actions_n; i++)
        if (!cache.actions[i].prompt || !*cache.actions[i].prompt)
            text_appendf(who, sizeof who, &at, "%s%s", at ? ", " : "",
                         cache.actions[i].name);
    if (at) {
        snprintf(out, size, "nothing to run: %s", who);
        return 1;
    }

    for (int i = 0; i < cache.pipelines_n; i++) {
        if (!cache.pipelines[i].actions_n) {
            snprintf(out, size, "the %s pipeline runs nothing",
                     cache.pipelines[i].name);
            return 1;
        }
        if (boardcfg_action(cache.pipelines[i].name)) {
            snprintf(out, size, "%s is both an action and a pipeline",
                     cache.pipelines[i].name);
            return 1;
        }
        for (int j = 0; j < cache.pipelines[i].actions_n; j++)
            if (!boardcfg_action(cache.pipelines[i].actions[j])) {
                snprintf(out, size, "the %s pipeline runs %s, and no action is",
                         cache.pipelines[i].name, cache.pipelines[i].actions[j]);
                return 1;
            }
    }

    return 0;
}

const struct board_action *boardcfg_action(const char *name)
{
    if (!name || !*name)
        return NULL;
    load();
    for (int i = 0; i < cache.actions_n; i++)
        if (!strcmp(serving_actions[i].name, name))
            return &serving_actions[i];
    return NULL;
}

int boardcfg_actions(const char **out, int max)
{
    load();
    int n = 0;
    for (int i = 0; i < cache.actions_n && n < max; i++)
        out[n++] = serving_actions[i].name;
    return n;
}

const struct board_pipeline *boardcfg_pipeline(const char *name)
{
    if (!name || !*name)
        return NULL;
    load();
    for (int i = 0; i < cache.pipelines_n; i++)
        if (!strcmp(cache.pipelines[i].name, name))
            return &cache.pipelines[i];
    return NULL;
}

int boardcfg_pipelines(const char **out, int max)
{
    load();
    int n = 0;
    for (int i = 0; i < cache.pipelines_n && n < max; i++)
        out[n++] = cache.pipelines[i].name;
    return n;
}

int boardcfg_for_backend(const char *name_of, const char *backend,
                         const char *tier, struct board_action *out)
{
    const struct board_action *p = boardcfg_action(name_of);
    if (!p || !out)
        return 0;

    *out = *p;

    int named = backend && *backend && strcmp(backend, cache.serving);
    int levelled = tier && *tier && strcmp(tier, p->tier);
    if (!named && !levelled)
        return 1;

    const char *name = named ? backend
                             : (cache.serving[0] ? cache.serving : "claude");
    const struct board_backend *b = backend_of(&cache, name);
    if (!b)
        return 1;

    enum board_tier at = tier_or_med(levelled ? tier : p->tier);

    snprintf(out->backend, sizeof out->backend, "%s", name);
    snprintf(out->tier, sizeof out->tier, "%s", boardcfg_tier_name(at));
    snprintf(out->model, sizeof out->model, "%s", b->level[at].model);
    snprintf(out->effort, sizeof out->effort, "%s", b->level[at].effort);
    return 1;
}

void boardcfg_levels_line(const struct board_backend *b, char *out, size_t size)
{
    size_t at = 0;
    if (!size)
        return;
    out[0] = '\0';
    for (int t = 0; t < BOARD_TIERS; t++)
        text_appendf(out, size, &at, "%s%s", t ? " \xc2\xb7 " : "",
                     b->level[t].model[0] ? b->level[t].model : "default");
}

static int argv_pair(char **out, int n, int max, const char *flag, const char *value)
{
    if (n + 2 > max)
        return n;
    out[n++] = (char *)flag;
    out[n++] = (char *)value;
    return n;
}

int boardcfg_argv(const struct board_action *p, const char *prompt, char **out,
                  int max)
{
    if (!p || !prompt || max < BOARDCFG_ARGV_MAX)
        return 0;

    int flags = max - 2;
    int n = 0;
    out[n++] = (char *)sessionfork_program();
    n = argv_pair(out, n, flags, "-b", p->backend[0] ? p->backend : "claude");
    if (p->model[0] && strcmp(p->model, "default"))
        n = argv_pair(out, n, flags, "-m", p->model);
    if (p->effort[0] && strcmp(p->effort, "default"))
        n = argv_pair(out, n, flags, "-e", p->effort);
    out[n++] = (char *)prompt;
    out[n] = NULL;
    return n;
}

const char *boardcfg_serving(void)
{
    load();
    return cache.serving[0] ? cache.serving : "claude";
}

const char *boardcfg_view(void)
{
    load();
    return cache.view[0] ? cache.view : "list";
}

int boardcfg_set_view(const char *view)
{
    if (!view || !*view)
        return 0;
    load();
    snprintf(cache.view, sizeof cache.view, "%s", view);
    return write_settings(&cache);
}

int boardcfg_set_serving(const char *backend)
{
    if (!backend || !*backend)
        return 0;
    load();
    if (!backend_of(&cache, backend))
        return 0;

    snprintf(cache.serving, sizeof cache.serving, "%s", backend);
    resolve();
    return write_settings(&cache);
}

struct board_cfg *boardcfg_copy(void)
{
    load();
    struct board_cfg *c = malloc(sizeof *c);
    if (!c)
        return NULL;
    *c = cache;
    for (int i = 0; i < c->actions_n; i++)
        c->actions[i].prompt = dup_or_null(cache.actions[i].prompt);
    return c;
}

void boardcfg_free(struct board_cfg *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->actions_n; i++)
        free(c->actions[i].prompt);
    free(c);
}

int boardcfg_set(const struct board_cfg *c)
{
    if (!c)
        return 0;
    load();

    cache.workers = c->workers;
    cache.auto_pull = c->auto_pull;
    cache.auto_pick = c->auto_pick;
    cache.archive_after = c->archive_after;
    cache.closed_shown = c->closed_shown;
    cache.open_shown = c->open_shown;

    snprintf(cache.serving, sizeof cache.serving, "%s", c->serving);
    snprintf(cache.view, sizeof cache.view, "%s", c->view);
    cache.backends_n = c->backends_n;
    memcpy(cache.backends, c->backends, sizeof cache.backends);
    resolve();
    return write_out(&cache);
}
