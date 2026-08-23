#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "mdcfg.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define CFG_MAX_BYTES (1u << 20)

static const char *const WHO_NAMES[BOARD_WHO] = {"triage", "worker", "audit",
                                                 "sweep", "merge"};

const char *boardcfg_who_name(enum board_who who)
{
    if (who < 0 || who >= BOARD_WHO)
        return WHO_NAMES[BOARD_WHO_TRIAGE];
    return WHO_NAMES[who];
}

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

static const char TRIAGE_PROMPT[] =
    "You are sorting one card on a work board. Read it and answer with JSON only, no prose and no code fence.\n"
    "\n"
    "  {\"kind\":\"...\",\"title\":\"...\",\"spec\":\"...\",\"cwd\":\"...\",\"confidence\":0.0,\"question\":\"\"}\n"
    "\n"
    "{kinds}"
    "\n"
    "title is a short name for the card, under 60 characters.\n"
    "spec is what the card asks for, in a few sentences. Do not invent\n"
    "  requirements the card does not imply.\n"
    "cwd is the absolute path of the repo it belongs to.\n"
    "confidence is 0.0 to 1.0.\n"
    "question is the one thing you would have to ask, or \"\".\n"
    "\n"
    "Almost every card can be placed. You are deciding two things only: which\n"
    "kind it is, and which repo it belongs to. You are NOT deciding how the\n"
    "work should be done.\n"
    "\n"
    "A card that says what it wants but not how is a normal card. Missing\n"
    "detail is not a reason to be unsure -- whoever picks the card up will\n"
    "work the detail out, and asking them to specify it up front defeats the\n"
    "point of capturing a thought quickly. Do not ask which component, which\n"
    "approach, how something should behave, or what a word meant if the\n"
    "sentence is plain. Set confidence high and write the spec from what the\n"
    "card actually says.\n"
    "\n"
    "todo, data and reference are notes to keep. They need no understanding at\n"
    "all: a number with no context is still data, a link with no explanation\n"
    "is still reference. Never ask what a note is for -- file it.\n"
    "\n"
    "The card was captured in a directory, given below. Unless the card names\n"
    "somewhere else, that is the repo, and you should say so rather than\n"
    "leaving cwd empty.\n"
    "\n"
    "Ask only when the card names no subject you could point at, or leans on\n"
    "context that is not written in the card. Not when it names a subject and\n"
    "leaves the details open -- that is most cards.\n"
    "\n"
    "  \"fix the thing with the tabs\"       place it. Tabs are a thing here.\n"
    "  \"make the retry logic exponential\"  place it. Retry logic is a thing.\n"
    "  \"cachegrind: 4.2ms warm\"            place it. Data needs no subject.\n"
    "  \"the board should remember filters\" place it. Says what it wants.\n"
    "  \"it's broken again\"                 ask. Nothing at all is named.\n"
    "  \"do the thing we talked about\"      ask. The subject is not in the card.\n"
    "  \"sync\"                              ask. A word, not a card.\n"
    "\n"
    "When you ask, set confidence below 0.5. Otherwise set it above 0.7 and\n"
    "leave question empty";

#define ALL_STEPS ((1u << BOARD_STEPS) - 1u)

static const char MERGE_PROMPT[] =
    "This branch could not be landed. What the attempt said is below.\n"
    "\n"
    "Rebase onto the branch it is going back to, resolve whatever is in the "
    "way, and make the check pass. Commit the result on the branch you are "
    "already on, then stop and say what you had to change.\n"
    "\n"
    "Do not merge it yourself: the board lands it once the branch is clean.";

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

static const struct {
    const char *name, *means, *prompt;
    int         priority;
    unsigned    steps;
} KINDS[] = {
    {"todo", "something the person means to do, off the computer or on it",
     "Use the wiki skill to add this to the todo list. Add it, say where it "
     "went, and stop. Do not do the thing itself.", 0, 0},

    {"data", "a measurement, a number, a result, a scrap worth keeping",
     "Use the wiki skill to file this where it belongs, compiling it into the "
     "article it bears on rather than leaving it loose. Say where it went and "
     "stop.", 0, 0},

    {"reference", "a link, a name, a fact, something to look up again later",
     "Use the wiki skill to file this as reference. Follow a link if there is "
     "one and write down what it actually says, rather than filing the bare "
     "URL. Say where it went and stop.", 0, 0},

    {"feature", "something that should exist and does not", "", 1, ALL_STEPS},
    {"bug", "something that exists and is wrong", "", 2, ALL_STEPS},
    {"chore", "upkeep: a rename, a bump, a cleanup", "", 0, ALL_STEPS},
};

static const char WORKER_PROMPT[] =
    "You are working one card from a board, alone, in a worktree of your own "
    "and on a branch of its own.\n"
    "\n"
    "Reach a state someone else can test, commit it to the branch you are "
    "already on, and then stop and say how to test it. Commit even when the "
    "work is unfinished: uncommitted work does not exist to anything "
    "downstream.\n"
    "\n"
    "Do not merge, do not switch branches, do not touch the main branch, and "
    "do not start work the card does not ask for.\n"
    "\n"
    "The CLAUDE.md files in scope are binding, not advisory. Two rules they "
    "state are broken most often, so they are repeated here as tests you can "
    "apply to your own diff before you commit:\n"
    "\n"
    "Comments. Default to none. A comment may record why something is as it "
    "is -- a constraint, a trap, a decision that looks wrong and is not. It "
    "may not say what the code does; the code says that. Before keeping one, "
    "delete it and ask what a reader lost: if the answer is nothing, leave it "
    "deleted. No rhetorical framing, no restating the signature, no explaining "
    "the obvious. Brief and technical.\n"
    "\n"
    "Commits. Read the last twenty messages in the log and write like them. "
    "The subject is `area: what changed`, lower case, no trailing full stop, "
    "naming the change rather than passing judgement on it: `sessions: close "
    "keeps the list open` and not `sessions: make closing behave sensibly`. "
    "Add a body only where the subject cannot carry it, and then it is more "
    "of what changed -- the functions, files and behaviour added, removed or "
    "replaced -- not an argument for the change, not what the reader gains, "
    "and not an account of how you worked. No co-author trailers and no "
    "attribution to a tool.";

static const char AUDIT_PROMPT[] =
    "Review the change on this branch against the commit it branched from. "
    "Answer with JSON only, no prose and no code fence:\n"
    "\n"
    "  {\"clean\":true,\"findings\":[]}\n"
    "  {\"clean\":false,\"findings\":[\"src/a.c: frees buf twice on the error path\"]}\n"
    "\n"
    "Look for: a mechanism duplicated that should be one, structure that "
    "fights the code around it, memory handled wrongly, and anything with a "
    "security cost.\n"
    "\n"
    "Also look for the repo's own rules being broken. The CLAUDE.md files in "
    "scope state them; breaking one is a finding however small it looks. "
    "Comments that say what the code does rather than why, comments that "
    "could be deleted without a reader losing anything, and commit messages "
    "carrying tool attribution are the usual ones.\n"
    "\n"
    "A finding is something you would stop the merge for, written as one "
    "sentence naming the file. Naming and taste are not findings; a stated "
    "rule is not taste. If "
    "there are none, say clean and mean it -- a gate that never opens is a "
    "gate nobody keeps.";

static const char SWEEP_PROMPT[] =
    "Look over this repo for what incremental work leaves behind. Answer with "
    "JSON only, no prose and no code fence:\n"
    "\n"
    "  {\"cards\":[\"...\",\"...\"]}\n"
    "\n"
    "Each card is a proposal a person reads before it goes on the board, one "
    "sentence: what should change, and where. Look for two "
    "mechanisms doing one job, a thing done three different ways, a helper "
    "copied instead of shared, and structure that has drifted from what the "
    "code around it does.\n"
    "\n"
    "Only what you would actually spend an afternoon on. Not style, not "
    "naming, not anything you would call a nitpick. Five at the very most, "
    "and an empty list is a fine answer -- a sweep that always finds "
    "something is one nobody will read twice.";

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

    if (strcmp(name, "claude"))
        return;
    snprintf(b->level[BOARD_TIER_LOW].model, sizeof b->level[0].model, "haiku");
    snprintf(b->level[BOARD_TIER_MED].model, sizeof b->level[0].model, "opus");
    snprintf(b->level[BOARD_TIER_HIGH].model, sizeof b->level[0].model, "opus");
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

    c->kinds_n = (int)(sizeof KINDS / sizeof *KINDS);
    if (c->kinds_n > BOARD_KINDS_MAX)
        c->kinds_n = BOARD_KINDS_MAX;
    for (int i = 0; i < c->kinds_n; i++) {
        snprintf(c->kinds[i].name, sizeof c->kinds[i].name, "%s", KINDS[i].name);
        c->kinds[i].means = dup_or_null(KINDS[i].means);
        c->kinds[i].prompt = dup_or_null(KINDS[i].prompt);
        c->kinds[i].priority = KINDS[i].priority;
        c->kinds[i].steps = KINDS[i].steps;
    }

    snprintf(c->serving, sizeof c->serving, "claude");
    for (const char *const *b = backend_names(); *b && c->backends_n < BOARD_BACKENDS_MAX; b++)
        backend_defaults(&c->backends[c->backends_n++], *b);

    for (int i = 0; i < BOARD_WHO; i++)
        snprintf(c->who[i].backend, sizeof c->who[i].backend, "claude");

    snprintf(c->who[BOARD_WHO_TRIAGE].model, sizeof c->who[BOARD_WHO_TRIAGE].model, "haiku");
    snprintf(c->who[BOARD_WHO_TRIAGE].effort, sizeof c->who[BOARD_WHO_TRIAGE].effort, "low");
    snprintf(c->who[BOARD_WHO_AUDIT].effort, sizeof c->who[BOARD_WHO_AUDIT].effort, "high");

    static const enum board_tier WHO_TIERS[BOARD_WHO] = {
        [BOARD_WHO_TRIAGE] = BOARD_TIER_LOW,
        [BOARD_WHO_WORKER] = BOARD_TIER_MED,
        [BOARD_WHO_AUDIT]  = BOARD_TIER_HIGH,
        [BOARD_WHO_SWEEP]  = BOARD_TIER_MED,
        [BOARD_WHO_MERGE]  = BOARD_TIER_MED,
    };
    for (int i = 0; i < BOARD_WHO; i++)
        snprintf(c->who[i].tier, sizeof c->who[i].tier, "%s",
                 boardcfg_tier_name(WHO_TIERS[i]));

    c->who[BOARD_WHO_MERGE].prompt = dup_or_null(MERGE_PROMPT);
    c->who[BOARD_WHO_TRIAGE].prompt = dup_or_null(TRIAGE_PROMPT);
    c->who[BOARD_WHO_WORKER].prompt = dup_or_null(WORKER_PROMPT);
    c->who[BOARD_WHO_AUDIT].prompt = dup_or_null(AUDIT_PROMPT);
    c->who[BOARD_WHO_SWEEP].prompt = dup_or_null(SWEEP_PROMPT);
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
    if (!who)
        return;
    for (int i = 0; i < BOARD_WHO; i++) {
        const cJSON *p = cJSON_GetObjectItem((cJSON *)who, WHO_NAMES[i]);
        if (!p)
            continue;
        set_str(c->who[i].backend, sizeof c->who[i].backend, p, "backend");
        set_str(c->who[i].model, sizeof c->who[i].model, p, "model");
        set_str(c->who[i].effort, sizeof c->who[i].effort, p, "effort");
        const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)p, "prompt"));
        if (prompt) {
            free(c->who[i].prompt);
            c->who[i].prompt = strdup(prompt);
        }
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

/* A role file written before tiers keeps the tier the defaults gave it, so an
 * existing board follows the serving backend instead of staying on claude. */
static int read_roles(struct board_cfg *c)
{
    int aged = 0;

    for (int i = 0; i < BOARD_WHO; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", WHO_NAMES[i]))
            continue;

        struct mdcfg m;
        if (!mdcfg_load(&m, path))
            continue;

        if (mdcfg_has(&m, "tier"))
            snprintf(c->who[i].tier, sizeof c->who[i].tier, "%s", mdcfg_get(&m, "tier"));
        else
            aged = 1;

        const char *backend = mdcfg_get(&m, "backend");
        if (*backend)
            snprintf(c->who[i].backend, sizeof c->who[i].backend, "%s", backend);
        snprintf(c->who[i].model, sizeof c->who[i].model, "%s", mdcfg_get(&m, "model"));
        snprintf(c->who[i].effort, sizeof c->who[i].effort, "%s", mdcfg_get(&m, "effort"));

        if (m.body && *m.body) {
            char *kept = dup_or_null(m.body);
            if (kept) {
                free(c->who[i].prompt);
                c->who[i].prompt = kept;
            }
        }
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
    if (!found)
        return;

    for (int i = 0; i < c->kinds_n; i++) {
        free(c->kinds[i].means);
        free(c->kinds[i].prompt);
    }
    memset(c->kinds, 0, sizeof c->kinds);
    c->kinds_n = 0;

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

    char nums[6][32];
    snprintf(nums[0], sizeof nums[0], "%d", c->workers);
    snprintf(nums[1], sizeof nums[1], "%d", c->auto_pull);
    snprintf(nums[2], sizeof nums[2], "%d", c->audit_files);
    snprintf(nums[3], sizeof nums[3], "%d", c->audit_lines);
    snprintf(nums[4], sizeof nums[4], "%d", c->sweep_every);
    snprintf(nums[5], sizeof nums[5], "%d", c->archive_after);

    const char *keys[] = {"serving", "workers", "auto pull", "audit files",
                          "audit lines", "sweep every", "archive after", "check"};
    const char *vals[] = {c->serving, nums[0], nums[1], nums[2], nums[3],
                          nums[4], nums[5], c->verify};

    return mdcfg_write(path, keys, vals, 8,
        "serving is the backend every tiered role runs on.\n"
        "workers is how many may run at once.\n"
        "auto pull is 1 to start backlog cards on a free worker, 0 to wait to\n"
        "be told; a card in review keeps its worker until you take it.\n"
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
    for (int i = 0; i < BOARD_WHO; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", WHO_NAMES[i])) {
            ok = 0;
            continue;
        }
        const char *keys[] = {"tier", "backend", "model", "effort"};
        const char *vals[] = {c->who[i].tier, c->who[i].backend, c->who[i].model,
                              c->who[i].effort};
        if (!mdcfg_write(path, keys, vals, 4, c->who[i].prompt))
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
static struct board_profile serving_who[BOARD_WHO];

/* serving_who borrows cache's prompt pointers and never owns them. */
static void resolve(void)
{
    const struct board_backend *b = boardcfg_backend(&cache, cache.serving);

    for (int i = 0; i < BOARD_WHO; i++) {
        serving_who[i] = cache.who[i];

        enum board_tier tier = boardcfg_tier_from_name(cache.who[i].tier);
        if (tier >= BOARD_TIERS)
            continue;

        snprintf(serving_who[i].backend, sizeof serving_who[i].backend, "%s",
                 cache.serving[0] ? cache.serving : "claude");
        snprintf(serving_who[i].model, sizeof serving_who[i].model, "%s",
                 b ? b->level[tier].model : "");
        snprintf(serving_who[i].effort, sizeof serving_who[i].effort, "%s",
                 b ? b->level[tier].effort : "");
    }
}

static void read_all(void)
{
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

const struct board_cfg *boardcfg(void)
{
    load();
    return &cache;
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

const struct board_profile *boardcfg_for(enum board_who who)
{
    load();
    if (who < 0 || who >= BOARD_WHO)
        who = BOARD_WHO_TRIAGE;
    return &serving_who[who];
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
    for (int i = 0; i < BOARD_WHO; i++)
        c->who[i].prompt = dup_or_null(cache.who[i].prompt);
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
    for (int i = 0; i < BOARD_WHO; i++)
        free(c->who[i].prompt);
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

    for (int i = 0; i < BOARD_WHO; i++) {
        char *kept = dup_or_null(c->who[i].prompt);
        free(cache.who[i].prompt);
        cache.who[i].prompt = kept;
    }
    cache.workers = c->workers;
    cache.auto_pull = c->auto_pull;
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
    for (int i = 0; i < BOARD_WHO; i++) {
        snprintf(cache.who[i].tier, sizeof cache.who[i].tier, "%s", c->who[i].tier);
        snprintf(cache.who[i].backend, sizeof cache.who[i].backend, "%s", c->who[i].backend);
        snprintf(cache.who[i].model, sizeof cache.who[i].model, "%s", c->who[i].model);
        snprintf(cache.who[i].effort, sizeof cache.who[i].effort, "%s", c->who[i].effort);
    }
    resolve();
    return write_out(&cache);
}
