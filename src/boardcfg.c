#include "boardcfg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    c->done_shown = 20;
    c->backlog_shown = 20;
    snprintf(c->projects, sizeof c->projects, "~/working");
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

/* Kinds and actions used to be copied into the config as markdown. They are
 * compiled in now, so the copies are stale the moment the binary changes. */
static void drop_stale(const char *leaf)
{
    char base[4096];
    if (!path_config_dir(base, sizeof base))
        return;

    char dir[4200];
    if ((size_t)snprintf(dir, sizeof dir, "%s/%s", base, leaf) >= sizeof dir)
        return;

    char names[BOARD_KINDS_MAX * 4][MDCFG_NAME];
    int  n = mdcfg_list(dir, names, BOARD_KINDS_MAX * 4);
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

/* The name a compiled-in file stands under: kinds/plan.md is the plan kind. */
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
    char copy[256];
    snprintf(copy, sizeof copy, "%s", list ? list : "");

    int n = 0;
    for (char *p = copy; *p && n < max;) {
        while (*p == ' ' || *p == ',')
            p++;
        char *start = p;
        while (*p && *p != ',')
            p++;
        char *end = p;
        while (end > start && end[-1] == ' ')
            end--;
        char kept = *end;
        *end = '\0';
        if (*start)
            snprintf(out[n++], BOARD_STEP_NAME, "%s", start);
        *end = kept;
    }
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
    c->done_shown = mdcfg_int(&m, "done shown", c->done_shown);
    c->backlog_shown = mdcfg_int(&m, "backlog shown", c->backlog_shown);

    const char *serving = mdcfg_get(&m, "serving");
    if (*serving)
        snprintf(c->serving, sizeof c->serving, "%s", serving);

    const char *view = mdcfg_get(&m, "view");
    if (*view)
        snprintf(c->view, sizeof c->view, "%s", view);

    if (mdcfg_has(&m, "projects"))
        snprintf(c->projects, sizeof c->projects, "%s", mdcfg_get(&m, "projects"));
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

static void read_kinds(struct board_cfg *c)
{
    for (int i = 0; i < defs_n && c->kinds_n < BOARD_KINDS_MAX; i++) {
        char name[MDCFG_NAME];
        if (!default_name(defs[i].path, "kinds", name, sizeof name))
            continue;

        struct mdcfg m;
        if (!mdcfg_parse(&m, strdup(defs[i].text)))
            continue;

        struct board_kind *k = &c->kinds[c->kinds_n++];
        memset(k, 0, sizeof *k);
        snprintf(k->name, sizeof k->name, "%s", name);
        k->means = dup_or_null(mdcfg_get(&m, "means"));
        k->priority = mdcfg_int(&m, "priority", 0);
        k->steps_n = names_of(mdcfg_get(&m, "steps"), k->steps,
                              BOARD_KIND_STEPS);
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
    snprintf(nums[4], sizeof nums[4], "%d", c->done_shown);
    snprintf(nums[5], sizeof nums[5], "%d", c->backlog_shown);

    const char *keys[] = {"serving", "view", "workers", "auto pull",
                          "auto pick", "archive after",
                          "done shown", "backlog shown", "projects"};
    const char *vals[] = {c->serving, c->view, nums[0], nums[1], nums[2],
                          nums[3], nums[4], nums[5], c->projects};

    return mdcfg_write(path, keys, vals, 9,
        "serving is the backend every tiered action runs on.\n"
        "view is list or grid: the board as rows, or as tiles in lanes.\n"
        "workers is how many may run at once.\n"
        "auto pull is 1 to start backlog cards on a free worker, 0 to wait to\n"
        "be told.\n"
        "auto pick is 1 to hand a worker the next backlog card when its current\n"
        "task completes, switching backends to match the card; 0 to leave it on\n"
        "the card until you take it.\n"
        "archive after is days a done card stays on the board; zero forever.\n"
        "done shown and backlog shown are how many cards those columns list;\n"
        "zero lists them all.\n"
        "projects is the directory the repos sit in; triage sets a card cwd\n"
        "from the project it names. Empty leaves the cwd it was captured in.\n"
        "\n"
        "The kinds a card can be, and the steps each one walks, are built into\n"
        "the binary and are not read from here.\n");
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
    for (int i = 0; i < cache.kinds_n; i++)
        free(cache.kinds[i].means);
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
    read_kinds(&cache);
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

/* The kinds and actions are compiled in, so anything wrong here is wrong in
 * board/ in the source tree, not in the config. */
int boardcfg_missing(char *out, size_t size)
{
    load();

    if (!cache.kinds_n) {
        snprintf(out, size, "no kinds are built into this binary");
        return 1;
    }

    if (!cache.actions_n) {
        snprintf(out, size, "no actions are built into this binary");
        return 1;
    }

    char   who[128] = "";
    size_t at = 0;
    for (int i = 0; i < cache.actions_n; i++)
        if (!cache.actions[i].prompt || !*cache.actions[i].prompt)
            at += (size_t)snprintf(who + at, sizeof who - at, "%s%s",
                                   at ? ", " : "", cache.actions[i].name);
    if (at) {
        snprintf(out, size, "nothing to run: %s", who);
        return 1;
    }

    for (int i = 0; i < cache.kinds_n; i++)
        for (int j = 0; j < cache.kinds[i].steps_n; j++)
            if (!boardcfg_action(cache.kinds[i].steps[j])) {
                snprintf(out, size, "%s takes the %s step, and no action runs it",
                         cache.kinds[i].name, cache.kinds[i].steps[j]);
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

const struct board_kind *boardcfg_kind(const char *name)
{
    if (!name || !*name)
        return NULL;
    const struct board_cfg *c = boardcfg();
    for (int i = 0; i < c->kinds_n; i++)
        if (!strcmp(c->kinds[i].name, name))
            return &c->kinds[i];
    return NULL;
}

int boardcfg_priority(const char *kind)
{
    const struct board_kind *k = boardcfg_kind(kind);
    return k ? k->priority : 0;
}

int boardcfg_kind_step_at(const char *kind, const char *step)
{
    const struct board_kind *k = boardcfg_kind(kind);
    if (!k || !step || !*step)
        return -1;
    for (int i = 0; i < k->steps_n; i++)
        if (!strcmp(k->steps[i], step))
            return i;
    return -1;
}

const char *boardcfg_kind_step(const char *kind, int at)
{
    const struct board_kind *k = boardcfg_kind(kind);
    if (!k || at < 0 || at >= k->steps_n)
        return NULL;
    return k->steps[at];
}

void boardcfg_kinds_block(char *out, size_t size)
{
    const struct board_cfg *c = boardcfg();
    size_t                  at = 0;

    at += (size_t)snprintf(out + at, size - at, "kind is one of:");
    for (int i = 0; i < c->kinds_n && at < size; i++)
        at += (size_t)snprintf(out + at, size - at, "%s %s", i ? "," : "",
                               c->kinds[i].name);
    if (at < size)
        at += (size_t)snprintf(out + at, size - at, ".\n");

    int wide = 0;
    for (int i = 0; i < c->kinds_n; i++) {
        int n = (int)strlen(c->kinds[i].name);
        if (n > wide)
            wide = n;
    }
    for (int i = 0; i < c->kinds_n && at < size; i++)
        at += (size_t)snprintf(out + at, size - at, "  %-*s  %s\n", wide,
                               c->kinds[i].name,
                               c->kinds[i].means ? c->kinds[i].means : "");
}

#define PROJECTS_MAX 128
#define PROJECT_NAME 128

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void boardcfg_projects_block(char *out, size_t size)
{
    if (!size)
        return;
    out[0] = '\0';

    const struct board_cfg *c = boardcfg();
    if (!c->projects[0])
        return;

    char       *full = path_expand_home(c->projects);
    const char *root = full ? full : c->projects;

    DIR *d = opendir(root);
    if (!d) {
        free(full);
        return;
    }

    char names[PROJECTS_MAX][PROJECT_NAME];
    int  found = 0;

    const struct dirent *e;
    while ((e = readdir(d)) && found < PROJECTS_MAX) {
        if (e->d_name[0] == '.')
            continue;

        char path[4096];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", root, e->d_name) >= sizeof path)
            continue;

        struct stat st;
        if (stat(path, &st) || !S_ISDIR(st.st_mode))
            continue;
        if ((size_t)snprintf(names[found], PROJECT_NAME, "%s", e->d_name) >= PROJECT_NAME)
            continue;
        found++;
    }
    closedir(d);

    if (!found) {
        free(full);
        return;
    }
    qsort(names, (size_t)found, PROJECT_NAME, by_name);

    size_t at = (size_t)snprintf(out, size, "projects:\n");
    for (int i = 0; i < found; i++) {
        int n = snprintf(NULL, 0, "  %s/%s\n", root, names[i]);
        if (at + (size_t)n >= size)
            break;
        at += (size_t)snprintf(out + at, size - at, "  %s/%s\n", root, names[i]);
    }
    free(full);
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

const struct board_action *boardcfg_for_backend(const char *name_of,
                                                const char *backend,
                                                const char *tier)
{
    static struct board_action out;

    const struct board_action *p = boardcfg_action(name_of);
    if (!p)
        return NULL;

    int named = backend && *backend && strcmp(backend, cache.serving);
    int levelled = tier && *tier && strcmp(tier, p->tier);
    if (!named && !levelled)
        return p;

    const char *name = named ? backend
                             : (cache.serving[0] ? cache.serving : "claude");
    const struct board_backend *b = backend_of(&cache, name);
    if (!b)
        return p;

    enum board_tier at = tier_or_med(levelled ? tier : p->tier);

    out = *p;
    snprintf(out.backend, sizeof out.backend, "%s", name);
    snprintf(out.tier, sizeof out.tier, "%s", boardcfg_tier_name(at));
    snprintf(out.model, sizeof out.model, "%s", b->level[at].model);
    snprintf(out.effort, sizeof out.effort, "%s", b->level[at].effort);
    return &out;
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
    for (int i = 0; i < c->kinds_n; i++)
        c->kinds[i].means = dup_or_null(cache.kinds[i].means);
    return c;
}

void boardcfg_free(struct board_cfg *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->actions_n; i++)
        free(c->actions[i].prompt);
    for (int i = 0; i < c->kinds_n; i++)
        free(c->kinds[i].means);
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
    cache.done_shown = c->done_shown;
    cache.backlog_shown = c->backlog_shown;

    snprintf(cache.projects, sizeof cache.projects, "%s", c->projects);
    snprintf(cache.serving, sizeof cache.serving, "%s", c->serving);
    snprintf(cache.view, sizeof cache.view, "%s", c->view);
    cache.backends_n = c->backends_n;
    memcpy(cache.backends, c->backends, sizeof cache.backends);
    resolve();
    return write_out(&cache);
}
