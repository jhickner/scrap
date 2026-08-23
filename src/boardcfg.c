#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "mdcfg.h"
#include "sessionfork.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define CFG_MAX_BYTES (1u << 20)

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

#define ALL_STEPS ((1u << BOARD_STEPS) - 1u)

static const char *const STEP_NAMES[BOARD_STEPS] = {
    "worktree", "review", "audit", "merge",
};

const char *boardcfg_step_name(enum board_step step)
{
    if (step < 0 || step >= BOARD_STEPS)
        return "";
    return STEP_NAMES[step];
}

enum board_step boardcfg_step_from_name(const char *name)
{
    if (name)
        for (int i = 0; i < BOARD_STEPS; i++)
            if (!strcmp(name, STEP_NAMES[i]))
                return (enum board_step)i;
    return BOARD_STEPS;
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

/* What a role's file leaves unsaid: it does the job it is named after, stands
 * in the step of that name if there is one, and a step a role stands in may be
 * stepped over by hand. */
static void role_defaults(struct board_profile *p)
{
    if (!p->does[0])
        snprintf(p->does, sizeof p->does, "%s", p->name);
    if (!p->tier[0])
        snprintf(p->tier, sizeof p->tier, "%s", boardcfg_tier_name(BOARD_TIER_MED));
    if (!p->step[0] && boardcfg_step_from_name(p->does) < BOARD_STEPS)
        snprintf(p->step, sizeof p->step, "%s", p->does);
    p->skippable = p->step[0] != '\0';
}

static void defaults(struct board_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->workers = 3;
    c->audit_files = 5;
    c->audit_lines = 200;
    c->sweep_every = 8;
    c->archive_after = 14;
    c->verify[0] = '\0';

    snprintf(c->serving, sizeof c->serving, "claude");
    for (const char *const *b = backend_names(); *b && c->backends_n < BOARD_BACKENDS_MAX; b++)
        backend_defaults(&c->backends[c->backends_n++], *b);
}

static void set_str(char *dst, size_t n, const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    if (s)
        snprintf(dst, n, "%s", s);
}

static void set_int(int *dst, const cJSON *o, const char *key)
{
    const cJSON *j = cJSON_GetObjectItem((cJSON *)o, key);
    if (j && cJSON_IsNumber(j))
        *dst = (int)j->valuedouble;
}

static void overlay(struct board_cfg *c, const cJSON *o)
{
    set_int(&c->workers, o, "workers");
    set_int(&c->auto_pull, o, "auto_pull");
    set_int(&c->auto_pick, o, "auto_pick");
    set_int(&c->audit_files, o, "audit_files");
    set_int(&c->audit_lines, o, "audit_lines");
    set_int(&c->sweep_every, o, "sweep_every");
    set_int(&c->archive_after, o, "archive_after");
    set_str(c->verify, sizeof c->verify, o, "verify");

    const cJSON *kinds = cJSON_GetObjectItem((cJSON *)o, "kinds");
    if (cJSON_IsArray(kinds)) {
        for (int i = 0; i < c->kinds_n; i++) {
            free(c->kinds[i].means);
            free(c->kinds[i].prompt);
        }
        memset(c->kinds, 0, sizeof c->kinds);
        c->kinds_n = 0;

        const cJSON *k = NULL;
        cJSON_ArrayForEach(k, kinds) {
            if (c->kinds_n >= BOARD_KINDS_MAX)
                break;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)k, "name"));
            if (!name || !*name)
                continue;
            struct board_kind *into = &c->kinds[c->kinds_n++];
            snprintf(into->name, sizeof into->name, "%s", name);
            const char *means = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)k, "means"));
            const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)k, "prompt"));
            into->means = dup_or_null(means ? means : "");
            into->prompt = dup_or_null(prompt ? prompt : "");
            set_int(&into->priority, k, "priority");

            const cJSON *steps = cJSON_GetObjectItem((cJSON *)k, "steps");
            if (!cJSON_IsArray(steps)) {
                into->steps = ALL_STEPS;
            } else {
                into->steps = 0;
                const cJSON *e = NULL;
                cJSON_ArrayForEach(e, steps) {
                    enum board_step at = boardcfg_step_from_name(cJSON_GetStringValue((cJSON *)e));
                    if (at < BOARD_STEPS)
                        into->steps |= 1u << at;
                }
            }
        }
    }

    const cJSON *who = cJSON_GetObjectItem((cJSON *)o, "who");
    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, who) {
        if (!p->string || !*p->string || c->roles_n >= BOARD_ROLES_MAX)
            continue;
        struct board_profile *into = &c->roles[c->roles_n++];
        snprintf(into->name, sizeof into->name, "%s", p->string);
        into->prompt = dup_or_null(
            cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)p, "prompt")));
        role_defaults(into);
    }
}

#define BOARD_DIR "board"

static int board_path(char *out, size_t size, const char *leaf, const char *name)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, leaf))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s.md", dir, name) < size;
}

static unsigned steps_of(const char *list)
{
    unsigned mask = 0;
    char     copy[256];
    snprintf(copy, sizeof copy, "%s", list);

    for (char *p = copy; *p;) {
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
        if (*start) {
            enum board_step at = boardcfg_step_from_name(start);
            if (at < BOARD_STEPS)
                mask |= 1u << at;
        }
        *end = kept;
    }
    return mask;
}

static void steps_str(unsigned mask, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < BOARD_STEPS && at < size; i++)
        if (mask & (1u << i))
            at += (size_t)snprintf(out + at, size - at, "%s%s", at ? ", " : "",
                                   boardcfg_step_name((enum board_step)i));
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
    c->audit_files = mdcfg_int(&m, "audit files", c->audit_files);
    c->audit_lines = mdcfg_int(&m, "audit lines", c->audit_lines);
    c->sweep_every = mdcfg_int(&m, "sweep every", c->sweep_every);
    c->archive_after = mdcfg_int(&m, "archive after", c->archive_after);

    const char *serving = mdcfg_get(&m, "serving");
    if (*serving)
        snprintf(c->serving, sizeof c->serving, "%s", serving);

    snprintf(c->verify, sizeof c->verify, "%s", mdcfg_get(&m, "check"));
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

        struct board_profile *p = &c->roles[c->roles_n++];
        snprintf(p->name, sizeof p->name, "%s", names[i]);
        snprintf(p->does, sizeof p->does, "%s", mdcfg_get(&m, "does"));
        snprintf(p->step, sizeof p->step, "%s", mdcfg_get(&m, "step"));

        const char *tier = mdcfg_get(&m, "tier");
        if (boardcfg_tier_from_name(tier) < BOARD_TIERS)
            snprintf(p->tier, sizeof p->tier, "%s", tier);
        else
            aged = 1;

        role_defaults(p);
        p->skippable = mdcfg_int(&m, "skippable", p->skippable);

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
        k->priority = mdcfg_int(&m, "priority", 0);
        k->steps = steps_of(mdcfg_get(&m, "steps"));
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
    snprintf(nums[3], sizeof nums[3], "%d", c->audit_files);
    snprintf(nums[4], sizeof nums[4], "%d", c->audit_lines);
    snprintf(nums[5], sizeof nums[5], "%d", c->sweep_every);
    snprintf(nums[6], sizeof nums[6], "%d", c->archive_after);

    const char *keys[] = {"serving", "workers", "auto pull", "auto pick",
                          "audit files", "audit lines", "sweep every",
                          "archive after", "check"};
    const char *vals[] = {c->serving, nums[0], nums[1], nums[2], nums[3],
                          nums[4], nums[5], nums[6], c->verify};

    return mdcfg_write(path, keys, vals, 9,
        "serving is the backend every tiered role runs on.\n"
        "workers is how many may run at once.\n"
        "auto pull is 1 to start backlog cards on a free worker, 0 to wait to\n"
        "be told.\n"
        "auto pick is 1 to hand a worker the next backlog card when its current\n"
        "task completes, switching backends to match the card; 0 to leave it on\n"
        "the card until you take it.\n"
        "A diff over either audit threshold is read before it lands; zero turns\n"
        "that half off.\n"
        "sweep every is cards landed in a repo before a sweep of it; zero never.\n"
        "archive after is days a done card stays on the board; zero forever.\n"
        "check is run in the worktree before a card lands, and it does not land\n"
        "if that fails.\n");
}

static int write_roles(const struct board_cfg *c)
{
    int ok = 1;
    for (int i = 0; i < c->roles_n; i++) {
        const struct board_profile *p = &c->roles[i];

        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", p->name)) {
            ok = 0;
            continue;
        }

        char        skip[8];
        snprintf(skip, sizeof skip, "%d", p->skippable);

        const char *keys[4], *vals[4];
        int         n = 0;
        keys[n] = "tier";
        vals[n++] = p->tier;
        if (strcmp(p->does, p->name)) {
            keys[n] = "does";
            vals[n++] = p->does;
        }
        if (p->step[0]) {
            keys[n] = "step";
            vals[n++] = p->step;
            keys[n] = "skippable";
            vals[n++] = skip;
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
        steps_str(c->kinds[i].steps, steps, sizeof steps);
        snprintf(priority, sizeof priority, "%d", c->kinds[i].priority);

        const char *keys[] = {"means", "priority", "steps"};
        const char *vals[] = {c->kinds[i].means ? c->kinds[i].means : "",
                              priority, steps};
        if (!mdcfg_write(path, keys, vals, 3, c->kinds[i].prompt))
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

static struct board_cfg cache;
static int              loaded;
static struct board_profile serving_roles[BOARD_ROLES_MAX];

static void resolve(void)
{
    const struct board_backend *b = boardcfg_backend(&cache, cache.serving);

    for (int i = 0; i < cache.roles_n; i++) {
        serving_roles[i] = cache.roles[i];

        enum board_tier tier = boardcfg_tier_from_name(cache.roles[i].tier);
        if (tier >= BOARD_TIERS)
            tier = BOARD_TIER_MED;

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
    }
}

static void read_all(void)
{
    cache_free();
    defaults(&cache);

    char old[4300];
    if (path_config_file(old, sizeof old, "board.json")) {
        char  *text = text_slurp(old, CFG_MAX_BYTES, NULL);
        cJSON *o = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (o) {
            overlay(&cache, o);
            cJSON_Delete(o);
            write_out(&cache);
            char aside[4400];
            snprintf(aside, sizeof aside, "%s.replaced", old);
            rename(old, aside);
            return;
        }
    }

    read_settings(&cache);
    int aged = read_roles(&cache);
    read_backends(&cache);
    read_kinds(&cache);

    char seed[4300];
    if (aged || (board_path(seed, sizeof seed, BOARD_DIR, "settings") && access(seed, F_OK)))
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
        for (int j = i + 1; j < cache.roles_n; j++)
            if (!strcmp(cache.roles[i].does, cache.roles[j].does)) {
                snprintf(out, size, "%s and %s both do %s", cache.roles[i].name,
                         cache.roles[j].name, cache.roles[i].does);
                return 1;
            }

    char   who[128] = "";
    size_t at = 0;
    for (int i = 0; i < cache.roles_n; i++)
        if (!cache.roles[i].prompt || !*cache.roles[i].prompt)
            at += (size_t)snprintf(who + at, sizeof who - at, "%s%s", at ? ", " : "",
                                   cache.roles[i].name);
    if (at) {
        snprintf(out, size, "no prompt in %s/roles: %s", dir, who);
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

int boardcfg_kind_takes(const char *kind, enum board_step step)
{
    if (step < 0 || step >= BOARD_STEPS)
        return 0;
    const struct board_kind *k = boardcfg_kind(kind);
    return k ? (k->steps & (1u << step)) != 0 : 1;
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

const struct board_profile *boardcfg_doing(const char *job)
{
    load();
    if (!job || !*job)
        return NULL;
    for (int i = 0; i < cache.roles_n; i++)
        if (!strcmp(serving_roles[i].does, job))
            return &serving_roles[i];
    return NULL;
}

const struct board_profile *boardcfg_for_step(enum board_step step)
{
    load();
    const char *name = boardcfg_step_name(step);
    if (!*name)
        return NULL;
    for (int i = 0; i < cache.roles_n; i++)
        if (!strcmp(serving_roles[i].step, name))
            return &serving_roles[i];
    return NULL;
}

const struct board_profile *boardcfg_doing_on(const char *job,
                                              const char *backend)
{
    static struct board_profile out;

    const struct board_profile *p = boardcfg_doing(job);
    if (!p || !backend || !*backend || !strcmp(backend, cache.serving))
        return p;

    const struct board_backend *b = boardcfg_backend(&cache, backend);
    if (!b)
        return p;

    enum board_tier tier = boardcfg_tier_from_name(p->tier);
    if (tier >= BOARD_TIERS)
        tier = BOARD_TIER_MED;

    out = *p;
    snprintf(out.backend, sizeof out.backend, "%s", backend);
    snprintf(out.model, sizeof out.model, "%s", b->level[tier].model);
    snprintf(out.effort, sizeof out.effort, "%s", b->level[tier].effort);
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

int boardcfg_argv(const struct board_profile *p, const char *prompt, char **out,
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
    cache.audit_files = c->audit_files;
    cache.audit_lines = c->audit_lines;
    cache.sweep_every = c->sweep_every;
    cache.archive_after = c->archive_after;

    for (int i = 0; i < cache.kinds_n; i++) {
        free(cache.kinds[i].means);
        free(cache.kinds[i].prompt);
    }
    memset(cache.kinds, 0, sizeof cache.kinds);
    cache.kinds_n = c->kinds_n;
    for (int i = 0; i < cache.kinds_n; i++) {
        cache.kinds[i] = c->kinds[i];
        cache.kinds[i].means = dup_or_null(c->kinds[i].means);
        cache.kinds[i].prompt = dup_or_null(c->kinds[i].prompt);
    }

    snprintf(cache.verify, sizeof cache.verify, "%s", c->verify);
    snprintf(cache.serving, sizeof cache.serving, "%s", c->serving);
    cache.backends_n = c->backends_n;
    memcpy(cache.backends, c->backends, sizeof cache.backends);
    resolve();
    return write_out(&cache);
}
