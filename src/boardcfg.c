#include "boardcfg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text.h"
#include "mdcfg.h"
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

enum board_tier boardcfg_tier_or_med(const char *name)
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

const struct board_backend *boardcfg_backend(const struct board_cfg *c,
                                             const char *name)
{
    if (!c || !name || !*name)
        return NULL;
    for (int i = 0; i < c->backends_n; i++)
        if (!strcmp(c->backends[i].name, name))
            return &c->backends[i];
    return NULL;
}

static const char *const RUNS_NAMES[BOARD_RUNS_MODES] = {
    "agent", "worker", "person", "command",
};

const char *boardcfg_runs_name(enum board_runs runs)
{
    if (runs < 0 || runs >= BOARD_RUNS_MODES)
        return RUNS_NAMES[BOARD_RUNS_AGENT];
    return RUNS_NAMES[runs];
}

enum board_runs boardcfg_runs_from_name(const char *name)
{
    if (name)
        for (int i = 0; i < BOARD_RUNS_MODES; i++)
            if (!strcmp(name, RUNS_NAMES[i]))
                return (enum board_runs)i;
    return BOARD_RUNS_AGENT;
}

static const char *const LOCK_NAMES[BOARD_LOCKS] = {"", "repo", "machine"};

const char *boardcfg_lock_name(enum board_lock lock)
{
    if (lock < 0 || lock >= BOARD_LOCKS)
        return LOCK_NAMES[BOARD_LOCK_NONE];
    return LOCK_NAMES[lock];
}

enum board_lock boardcfg_lock_from_name(const char *name)
{
    if (name)
        for (int i = 0; i < BOARD_LOCKS; i++)
            if (!strcmp(name, LOCK_NAMES[i]))
                return (enum board_lock)i;
    return BOARD_LOCK_NONE;
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
        snprintf(b->level[BOARD_TIER_LOW].model, sizeof b->level[0].model, "sonnet");
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

/* Fields a role file omits: job is the file's own name, and the step it stands
 * in is the job. */
static void role_defaults(struct board_role *p)
{
    if (!p->job[0])
        snprintf(p->job, sizeof p->job, "%s", p->name);
    if (!p->tier[0])
        snprintf(p->tier, sizeof p->tier, "%s", boardcfg_tier_name(BOARD_TIER_MED));
    if (!p->step[0])
        snprintf(p->step, sizeof p->step, "%s", p->job);
    if (!p->pass_label[0])
        snprintf(p->pass_label, sizeof p->pass_label, "approve");
    if (!p->fail_label[0])
        snprintf(p->fail_label, sizeof p->fail_label, "send back");
}

static void defaults(struct board_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->workers = 3;
    c->sweep_every = 8;
    c->archive_after = 14;
    c->done_shown = 20;
    c->backlog_shown = 20;
    snprintf(c->projects, sizeof c->projects, "~/working");

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

void boardcfg_kind_steps(struct board_kind *k, const char *list)
{
    char copy[256];
    snprintf(copy, sizeof copy, "%s", list);

    k->steps_n = 0;
    for (char *p = copy; *p && k->steps_n < BOARD_KIND_STEPS;) {
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
            snprintf(k->steps[k->steps_n++], BOARD_STEP_NAME, "%s", start);
        *end = kept;
    }
}

static void steps_str(const struct board_kind *k, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < k->steps_n && at < size; i++)
        at += (size_t)snprintf(out + at, size - at, "%s%s", at ? ", " : "",
                               k->steps[i]);
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
    c->sweep_every = mdcfg_int(&m, "sweep every", c->sweep_every);
    c->archive_after = mdcfg_int(&m, "archive after", c->archive_after);
    c->done_shown = mdcfg_int(&m, "done shown", c->done_shown);
    c->backlog_shown = mdcfg_int(&m, "backlog shown", c->backlog_shown);

    const char *serving = mdcfg_get(&m, "serving");
    if (*serving)
        snprintf(c->serving, sizeof c->serving, "%s", serving);

    if (mdcfg_has(&m, "projects"))
        snprintf(c->projects, sizeof c->projects, "%s", mdcfg_get(&m, "projects"));
    mdcfg_free(&m);
}

static int read_roles(struct board_cfg *c)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, BOARD_DIR "/roles"))
        return 0;

    char names[BOARD_ROLES_MAX][MDCFG_NAME];
    int  found = mdcfg_list(dir, names, BOARD_ROLES_MAX);
    int  aged = 0;

    for (int i = 0; i < found && c->roles_n < BOARD_ROLES_MAX; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", names[i]))
            continue;

        struct mdcfg m;
        if (!mdcfg_load(&m, path))
            continue;

        struct board_role *p = &c->roles[c->roles_n++];
        snprintf(p->name, sizeof p->name, "%s", names[i]);
        snprintf(p->job, sizeof p->job, "%s", mdcfg_get(&m, "job"));
        snprintf(p->step, sizeof p->step, "%s", mdcfg_get(&m, "step"));
        snprintf(p->fail_marker, sizeof p->fail_marker, "%s",
                 mdcfg_get(&m, "fail marker"));
        snprintf(p->fail_step, sizeof p->fail_step, "%s",
                 mdcfg_get(&m, "fail step"));
        snprintf(p->fail_prompt, sizeof p->fail_prompt, "%s",
                 mdcfg_get(&m, "fail prompt"));
        snprintf(p->pass_label, sizeof p->pass_label, "%s",
                 mdcfg_get(&m, "pass label"));
        snprintf(p->fail_label, sizeof p->fail_label, "%s",
                 mdcfg_get(&m, "fail label"));
        p->runs = boardcfg_runs_from_name(mdcfg_get(&m, "runs"));
        p->lock = boardcfg_lock_from_name(mdcfg_get(&m, "lock"));
        p->over_files = mdcfg_int(&m, "over files", 0);
        p->over_lines = mdcfg_int(&m, "over lines", 0);

        const char *tier = mdcfg_get(&m, "tier");
        if (boardcfg_tier_from_name(tier) < BOARD_TIERS)
            snprintf(p->tier, sizeof p->tier, "%s", tier);
        else
            aged = 1;

        role_defaults(p);
        p->skippable = mdcfg_int(&m, "skippable", 1);

        if (mdcfg_has(&m, "backend") || mdcfg_has(&m, "model") ||
            mdcfg_has(&m, "effort"))
            aged = 1;

        p->prompt = dup_or_null(m.body ? m.body : "");
        mdcfg_free(&m);
    }
    return aged;
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
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, BOARD_DIR "/kinds"))
        return;

    char names[BOARD_KINDS_MAX][MDCFG_NAME];
    int  found = mdcfg_list(dir, names, BOARD_KINDS_MAX);

    for (int i = 0; i < found; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/kinds", names[i]))
            continue;

        struct mdcfg m;
        if (!mdcfg_load(&m, path))
            continue;

        struct board_kind *k = &c->kinds[c->kinds_n++];
        snprintf(k->name, sizeof k->name, "%s", names[i]);
        k->means = dup_or_null(mdcfg_get(&m, "means"));
        k->prompt = dup_or_null(m.body ? m.body : "");
        k->approval_prompt = dup_or_null(mdcfg_get(&m, "approval prompt"));
        snprintf(k->next_kind, sizeof k->next_kind, "%s", mdcfg_get(&m, "next kind"));
        k->priority = mdcfg_int(&m, "priority", 0);
        boardcfg_kind_steps(k, mdcfg_get(&m, "steps"));
        mdcfg_free(&m);
    }
}

static int write_settings(const struct board_cfg *c)
{
    char path[4300];
    if (!board_path(path, sizeof path, BOARD_DIR, "settings"))
        return 0;

    char nums[7][32];
    snprintf(nums[0], sizeof nums[0], "%d", c->workers);
    snprintf(nums[1], sizeof nums[1], "%d", c->auto_pull);
    snprintf(nums[2], sizeof nums[2], "%d", c->auto_pick);
    snprintf(nums[3], sizeof nums[3], "%d", c->sweep_every);
    snprintf(nums[4], sizeof nums[4], "%d", c->archive_after);
    snprintf(nums[5], sizeof nums[5], "%d", c->done_shown);
    snprintf(nums[6], sizeof nums[6], "%d", c->backlog_shown);

    const char *keys[] = {"serving", "workers", "auto pull", "auto pick",
                          "sweep every", "archive after", "done shown",
                          "backlog shown", "projects"};
    const char *vals[] = {c->serving, nums[0], nums[1], nums[2], nums[3],
                          nums[4], nums[5], nums[6], c->projects};

    return mdcfg_write(path, keys, vals, 9,
        "serving is the backend every tiered role runs on.\n"
        "workers is how many may run at once.\n"
        "auto pull is 1 to start backlog cards on a free worker, 0 to wait to\n"
        "be told.\n"
        "auto pick is 1 to hand a worker the next backlog card when its current\n"
        "task completes, switching backends to match the card; 0 to leave it on\n"
        "the card until you take it.\n"
        "sweep every is cards landed in a repo before a sweep of it; zero never.\n"
        "archive after is days a done card stays on the board; zero forever.\n"
        "done shown and backlog shown are how many cards those columns list;\n"
        "zero lists them all.\n"
        "projects is the directory the repos sit in; triage sets a card cwd\n"
        "from the project it names. Empty leaves the cwd it was captured in.\n"
        "\n"
        "What each step of a card does is in board/roles; which steps a kind\n"
        "takes, and in what order, is in board/kinds.\n");
}

static int write_roles(const struct board_cfg *c)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, BOARD_DIR "/roles"))
        return 0;

    char had[BOARD_ROLES_MAX * 2][MDCFG_NAME];
    int  found = mdcfg_list(dir, had, BOARD_ROLES_MAX * 2);
    for (int i = 0; i < found; i++) {
        int still = 0;
        for (int j = 0; j < c->roles_n && !still; j++)
            still = !strcmp(c->roles[j].name, had[i]);
        char gone[4300];
        if (!still && board_path(gone, sizeof gone, BOARD_DIR "/roles", had[i]))
            unlink(gone);
    }

    int ok = 1;
    for (int i = 0; i < c->roles_n; i++) {
        const struct board_role *p = &c->roles[i];

        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", p->name)) {
            ok = 0;
            continue;
        }

        char skip[8], files[16], lines[16];
        snprintf(skip, sizeof skip, "%d", p->skippable);
        snprintf(files, sizeof files, "%d", p->over_files);
        snprintf(lines, sizeof lines, "%d", p->over_lines);

        const char *keys[12], *vals[12];
        int         n = 0;
        keys[n] = "runs";
        vals[n++] = boardcfg_runs_name(p->runs);
        keys[n] = "tier";
        vals[n++] = p->tier;
        if (strcmp(p->job, p->name)) {
            keys[n] = "job";
            vals[n++] = p->job;
        }
        if (strcmp(p->step, p->job)) {
            keys[n] = "step";
            vals[n++] = p->step;
        }
        keys[n] = "skippable";
        vals[n++] = skip;
        if (p->runs == BOARD_RUNS_AGENT && p->fail_marker[0]) {
            keys[n] = "fail marker";
            vals[n++] = p->fail_marker;
        }
        if (p->fail_step[0]) {
            keys[n] = "fail step";
            vals[n++] = p->fail_step;
        }
        if (p->fail_prompt[0]) {
            keys[n] = "fail prompt";
            vals[n++] = p->fail_prompt;
        }
        if (p->runs == BOARD_RUNS_PERSON) {
            keys[n] = "pass label";
            vals[n++] = p->pass_label;
            keys[n] = "fail label";
            vals[n++] = p->fail_label;
        }
        if (p->over_files || p->over_lines) {
            keys[n] = "over files";
            vals[n++] = files;
            keys[n] = "over lines";
            vals[n++] = lines;
        }
        if (p->lock != BOARD_LOCK_NONE) {
            keys[n] = "lock";
            vals[n++] = boardcfg_lock_name(p->lock);
        }

        if (!mdcfg_write(path, keys, vals, n, p->prompt))
            ok = 0;
    }
    return ok;
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

static int write_kinds(const struct board_cfg *c)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, BOARD_DIR "/kinds"))
        return 0;

    char names[BOARD_KINDS_MAX * 2][MDCFG_NAME];
    int  had = mdcfg_list(dir, names, BOARD_KINDS_MAX * 2);
    for (int i = 0; i < had; i++) {
        int still = 0;
        for (int j = 0; j < c->kinds_n && !still; j++)
            still = !strcmp(c->kinds[j].name, names[i]);
        if (still)
            continue;
        char gone[4300];
        if (board_path(gone, sizeof gone, BOARD_DIR "/kinds", names[i]))
            unlink(gone);
    }

    int ok = 1;
    for (int i = 0; i < c->kinds_n; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/kinds", c->kinds[i].name)) {
            ok = 0;
            continue;
        }
        char steps[128], priority[16];
        steps_str(&c->kinds[i], steps, sizeof steps);
        snprintf(priority, sizeof priority, "%d", c->kinds[i].priority);

        const char *keys[] = {"means", "priority", "steps", "approval prompt",
                              "next kind"};
        const char *vals[] = {c->kinds[i].means ? c->kinds[i].means : "",
                              priority, steps,
                              c->kinds[i].approval_prompt ? c->kinds[i].approval_prompt : "",
                              c->kinds[i].next_kind};
        if (!mdcfg_write(path, keys, vals, 5, c->kinds[i].prompt))
            ok = 0;
    }
    return ok;
}

static int write_out(const struct board_cfg *c)
{
    int ok = write_settings(c);
    if (!write_roles(c))
        ok = 0;
    if (!write_backends(c))
        ok = 0;
    if (!write_kinds(c))
        ok = 0;
    return ok;
}

static struct board_cfg  cache;
static int               loaded;
static struct board_role serving_roles[BOARD_ROLES_MAX];

#define STEPS_MAX (BOARD_KINDS_MAX * BOARD_KIND_STEPS)

static char        step_names[STEPS_MAX][BOARD_STEP_NAME];
static const char *steps[STEPS_MAX];
static int         steps_n;

static void order_steps(const struct board_cfg *c)
{
    steps_n = 0;
    for (int i = 0; i < c->kinds_n; i++) {
        for (int j = 0; j < c->kinds[i].steps_n; j++) {
            const char *name = c->kinds[i].steps[j];
            int         at = -1;
            for (int k = 0; k < steps_n && at < 0; k++)
                if (!strcmp(steps[k], name))
                    at = k;
            if (at >= 0)
                continue;
            if (steps_n >= STEPS_MAX)
                return;

            int before = steps_n;
            for (int k = j + 1; k < c->kinds[i].steps_n && before == steps_n; k++)
                for (int m = 0; m < steps_n; m++)
                    if (!strcmp(steps[m], c->kinds[i].steps[k])) {
                        before = m;
                        break;
                    }

            for (int m = steps_n; m > before; m--)
                steps[m] = steps[m - 1];
            snprintf(step_names[steps_n], BOARD_STEP_NAME, "%s", name);
            steps[before] = step_names[steps_n];
            steps_n++;
        }
    }
}

static void resolve(void)
{
    const struct board_backend *b = boardcfg_backend(&cache, cache.serving);

    for (int i = 0; i < cache.roles_n; i++) {
        serving_roles[i] = cache.roles[i];

        enum board_tier tier = boardcfg_tier_or_med(cache.roles[i].tier);

        snprintf(serving_roles[i].backend, sizeof serving_roles[i].backend, "%s",
                 cache.serving[0] ? cache.serving : "claude");
        snprintf(serving_roles[i].model, sizeof serving_roles[i].model, "%s",
                 b ? b->level[tier].model : "");
        snprintf(serving_roles[i].effort, sizeof serving_roles[i].effort, "%s",
                 b ? b->level[tier].effort : "");
    }
}

static void cache_free(void)
{
    for (int i = 0; i < cache.roles_n; i++)
        free(cache.roles[i].prompt);
    for (int i = 0; i < cache.kinds_n; i++) {
        free(cache.kinds[i].means);
        free(cache.kinds[i].prompt);
        free(cache.kinds[i].approval_prompt);
    }
}

static void read_all(void)
{
    cache_free();
    defaults(&cache);

    read_settings(&cache);
    int aged = read_roles(&cache);
    read_backends(&cache);
    read_kinds(&cache);
    order_steps(&cache);

    char seed[4300];
    if (aged || (board_path(seed, sizeof seed, BOARD_DIR, "settings") &&
                 access(seed, F_OK)))
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

int boardcfg_missing(char *out, size_t size)
{
    load();

    char full[4096], dir[4096];
    if (!mdcfg_dir(full, sizeof full, BOARD_DIR))
        return 0;
    path_home_relative(full, dir, sizeof dir);

    if (!cache.kinds_n) {
        snprintf(out, size, "no kinds in %s/kinds", dir);
        return 1;
    }

    if (!cache.roles_n) {
        snprintf(out, size, "no roles in %s/roles", dir);
        return 1;
    }

    for (int i = 0; i < cache.roles_n; i++)
        for (int j = i + 1; j < cache.roles_n; j++) {
            if (!strcmp(cache.roles[i].job, cache.roles[j].job)) {
                snprintf(out, size, "%s and %s both do %s", cache.roles[i].name,
                         cache.roles[j].name, cache.roles[i].job);
                return 1;
            }
            if (!strcmp(cache.roles[i].step, cache.roles[j].step)) {
                snprintf(out, size, "%s and %s both stand in %s",
                         cache.roles[i].name, cache.roles[j].name,
                         cache.roles[i].step);
                return 1;
            }
        }

    if (!boardcfg_worker()) {
        snprintf(out, size, "no role in %s/roles runs a worker", dir);
        return 1;
    }

    char   who[128] = "";
    size_t at = 0;
    for (int i = 0; i < cache.roles_n; i++)
        if (cache.roles[i].runs != BOARD_RUNS_PERSON &&
            (!cache.roles[i].prompt || !*cache.roles[i].prompt))
            at += (size_t)snprintf(who + at, sizeof who - at, "%s%s",
                                   at ? ", " : "", cache.roles[i].name);
    if (at) {
        snprintf(out, size, "nothing to run in %s/roles: %s", dir, who);
        return 1;
    }

    for (int i = 0; i < cache.kinds_n; i++)
        for (int j = 0; j < cache.kinds[i].steps_n; j++)
            if (!boardcfg_for_step(cache.kinds[i].steps[j])) {
                snprintf(out, size, "%s takes the %s step, and no role in "
                         "%s/roles stands in it", cache.kinds[i].name,
                         cache.kinds[i].steps[j], dir);
                return 1;
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

static const struct board_kind *kind_or_board(const char *kind)
{
    static struct board_kind board;
    const struct board_kind *k = boardcfg_kind(kind);
    if (k)
        return k;

    board.steps_n = 0;
    for (int i = 0; i < steps_n && board.steps_n < BOARD_KIND_STEPS; i++)
        snprintf(board.steps[board.steps_n++], BOARD_STEP_NAME, "%s", steps[i]);
    return &board;
}

int boardcfg_kind_step_at(const char *kind, const char *step)
{
    if (!step || !*step)
        return -1;
    const struct board_kind *k = kind_or_board(kind);
    for (int i = 0; i < k->steps_n; i++)
        if (!strcmp(k->steps[i], step))
            return i;
    return -1;
}

int boardcfg_kind_takes(const char *kind, const char *step)
{
    return boardcfg_kind_step_at(kind, step) >= 0;
}

const char *boardcfg_kind_step(const char *kind, int at)
{
    const struct board_kind *k = kind_or_board(kind);
    if (at < 0 || at >= k->steps_n)
        return NULL;
    return k->steps[at];
}

void boardcfg_kind_steps_default(struct board_kind *k)
{
    load();
    k->steps_n = 0;
    for (int i = 0; i < steps_n && k->steps_n < BOARD_KIND_STEPS; i++)
        snprintf(k->steps[k->steps_n++], BOARD_STEP_NAME, "%s", steps[i]);
}

int boardcfg_steps(const char *const **out)
{
    load();
    if (out)
        *out = steps;
    return steps_n;
}

char *boardcfg_expand(const char *text, const char *id)
{
    if (!text)
        return NULL;
    if (!id)
        id = "";

    const char *mark = "{id}";
    size_t      len = strlen(mark), grew = strlen(id);
    size_t      need = strlen(text) + 1;
    for (const char *at = text; (at = strstr(at, mark)); at += len)
        need += grew - len;

    char *out = malloc(need);
    if (!out)
        return NULL;

    size_t at = 0;
    for (const char *from = text;;) {
        const char *hit = strstr(from, mark);
        if (!hit) {
            memcpy(out + at, from, strlen(from) + 1);
            break;
        }
        memcpy(out + at, from, (size_t)(hit - from));
        at += (size_t)(hit - from);
        memcpy(out + at, id, grew);
        at += grew;
        from = hit + len;
    }
    return out;
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

const struct board_role *boardcfg_for_job(const char *job)
{
    load();
    if (!job || !*job)
        return NULL;
    for (int i = 0; i < cache.roles_n; i++)
        if (!strcmp(serving_roles[i].job, job))
            return &serving_roles[i];
    return NULL;
}

const struct board_role *boardcfg_for_step(const char *step)
{
    load();
    if (!step || !*step)
        return NULL;
    for (int i = 0; i < cache.roles_n; i++)
        if (!strcmp(serving_roles[i].step, step))
            return &serving_roles[i];
    return NULL;
}

const struct board_role *boardcfg_worker(void)
{
    load();
    for (int i = 0; i < cache.roles_n; i++)
        if (serving_roles[i].runs == BOARD_RUNS_WORKER)
            return &serving_roles[i];
    return NULL;
}

const struct board_role *boardcfg_for_backend(const char *job,
                                                 const char *backend,
                                                 const char *tier)
{
    static struct board_role out;

    const struct board_role *p = boardcfg_for_job(job);
    if (!p)
        return NULL;

    int named = backend && *backend && strcmp(backend, cache.serving);
    int levelled = tier && *tier && strcmp(tier, p->tier);
    if (!named && !levelled)
        return p;

    const char *name = named ? backend
                             : (cache.serving[0] ? cache.serving : "claude");
    const struct board_backend *b = boardcfg_backend(&cache, name);
    if (!b)
        return p;

    enum board_tier at = boardcfg_tier_or_med(levelled ? tier : p->tier);

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

int boardcfg_argv(const struct board_role *p, const char *prompt, char **out,
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

int boardcfg_set_serving(const char *backend)
{
    if (!backend || !*backend)
        return 0;
    load();
    if (!boardcfg_backend(&cache, backend))
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
    for (int i = 0; i < c->roles_n; i++)
        c->roles[i].prompt = dup_or_null(cache.roles[i].prompt);
    for (int i = 0; i < c->kinds_n; i++) {
        c->kinds[i].means = dup_or_null(cache.kinds[i].means);
        c->kinds[i].prompt = dup_or_null(cache.kinds[i].prompt);
        c->kinds[i].approval_prompt = dup_or_null(cache.kinds[i].approval_prompt);
    }
    return c;
}

void boardcfg_free(struct board_cfg *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->roles_n; i++)
        free(c->roles[i].prompt);
    for (int i = 0; i < c->kinds_n; i++) {
        free(c->kinds[i].means);
        free(c->kinds[i].prompt);
        free(c->kinds[i].approval_prompt);
    }
    free(c);
}

int boardcfg_set(const struct board_cfg *c)
{
    if (!c)
        return 0;
    load();

    for (int i = 0; i < cache.roles_n; i++)
        free(cache.roles[i].prompt);
    memset(cache.roles, 0, sizeof cache.roles);
    cache.roles_n = c->roles_n;
    for (int i = 0; i < cache.roles_n; i++) {
        cache.roles[i] = c->roles[i];
        cache.roles[i].prompt = dup_or_null(c->roles[i].prompt);
    }
    cache.workers = c->workers;
    cache.auto_pull = c->auto_pull;
    cache.auto_pick = c->auto_pick;
    cache.sweep_every = c->sweep_every;
    cache.archive_after = c->archive_after;
    cache.done_shown = c->done_shown;
    cache.backlog_shown = c->backlog_shown;

    for (int i = 0; i < cache.kinds_n; i++) {
        free(cache.kinds[i].means);
        free(cache.kinds[i].prompt);
        free(cache.kinds[i].approval_prompt);
    }
    memset(cache.kinds, 0, sizeof cache.kinds);
    cache.kinds_n = c->kinds_n;
    for (int i = 0; i < cache.kinds_n; i++) {
        cache.kinds[i] = c->kinds[i];
        cache.kinds[i].means = dup_or_null(c->kinds[i].means);
        cache.kinds[i].prompt = dup_or_null(c->kinds[i].prompt);
        cache.kinds[i].approval_prompt = dup_or_null(c->kinds[i].approval_prompt);
    }

    snprintf(cache.projects, sizeof cache.projects, "%s", c->projects);
    snprintf(cache.serving, sizeof cache.serving, "%s", c->serving);
    cache.backends_n = c->backends_n;
    memcpy(cache.backends, c->backends, sizeof cache.backends);
    resolve();
    return write_out(&cache);
}
