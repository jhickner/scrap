#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "mdcfg.h"
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

// Triage is told it may fail. A classifier given only a list of kinds will
// always pick one, and a confident wrong kind costs more than an admitted
// unknown: the first sends a worker somewhere, the second asks a question.
// Triage may fail, and saying so is a result. But the bar for failing is
// that the card names nothing to point at -- not that it leaves the work
// open, which is what a card thrown down in a hurry always does. Asked to
// flag anything ambiguous, a classifier asks how to build things.
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

// "Do not commit to the main branch" reads as "do not commit" often enough to
// matter: work left dirty in a worktree is work the merge queue cannot see, the
// audit cannot measure, and a reaped worker loses.
// The classes a card can fall into, and what a worker is told for each. All of
// it is configuration: a board whose work does not divide this way says so in
// board.json rather than in here.
//
// A wiki kind is a note to be filed, so it wants no branch and no review: the
// worker writes it where it goes and the card is done. Work kinds go through
// the gate.
#define ALL_STEPS ((1u << BOARD_STEPS) - 1u)

// Sent in when the queue could not land a card by itself: a rebase that
// conflicted, a check that failed, a merge that would not go. It is not asked
// to land the card -- the queue does that once the branch is clean again.
static const char MERGE_PROMPT[] =
    "This branch could not be landed. What the attempt said is below.\n"
    "\n"
    "Rebase onto the branch it is going back to, resolve whatever is in the "
    "way, and make the check pass. Commit the result on the branch you are "
    "already on, then stop and say what you had to change.\n"
    "\n"
    "Do not merge it yourself, do not switch branches, and do not touch the "
    "main branch: the board lands it once the branch is clean.";

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
    "do not start work the card does not ask for.";

// The audit is a gate, so it has to answer a question rather than write an
// essay: findings are things that would stop a merge, and everything else is
// clean.
static const char AUDIT_PROMPT[] =
    "Review the change on this branch against the commit it branched from. "
    "Answer with JSON only, no prose and no code fence:\n"
    "\n"
    "  {\"clean\":true,\"findings\":[]}\n"
    "\n"
    "Look for: a mechanism duplicated that should be one, structure that "
    "fights the code around it, memory handled wrongly, and anything with a "
    "security cost.\n"
    "\n"
    "A finding is something you would stop the merge for, written as one "
    "sentence naming the file. Style, naming and taste are not findings. If "
    "there are none, say clean and mean it -- a gate that never opens is a "
    "gate nobody keeps.";

// A sweep is looking for what no single card could show: each landed on its
// own and made sense on its own, and the duplication is only visible across
// them.
static const char SWEEP_PROMPT[] =
    "Look over this repo for what incremental work leaves behind. Answer with "
    "JSON only, no prose and no code fence:\n"
    "\n"
    "  {\"cards\":[\"...\",\"...\"]}\n"
    "\n"
    "Each card is one sentence: what should change, and where. Look for two "
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


static void defaults(struct board_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->workers = 3;
    c->usage_ceiling = 85;
    c->reset_hold = 10;
    c->audit_files = 5;
    c->audit_lines = 200;
    c->sweep_every = 8;
    c->archive_after = 14;
    snprintf(c->delegation, sizeof c->delegation, "claude,codex,grok");
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

    for (int i = 0; i < BOARD_WHO; i++)
        snprintf(c->who[i].backend, sizeof c->who[i].backend, "claude");

    // Triage is a classifier: it wants the cheap model and little thinking.
    snprintf(c->who[BOARD_WHO_TRIAGE].model, sizeof c->who[BOARD_WHO_TRIAGE].model, "haiku");
    snprintf(c->who[BOARD_WHO_TRIAGE].effort, sizeof c->who[BOARD_WHO_TRIAGE].effort, "low");
    snprintf(c->who[BOARD_WHO_AUDIT].effort, sizeof c->who[BOARD_WHO_AUDIT].effort, "high");

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

// The board.json this used to be kept in, read once so a board configured
// before the files existed carries over. Nothing writes it any more.
static void overlay(struct board_cfg *c, const cJSON *o)
{
    set_int(&c->workers, o, "workers");
    set_int(&c->usage_ceiling, o, "usage_ceiling");
    set_int(&c->reset_hold, o, "reset_hold");
    set_int(&c->audit_files, o, "audit_files");
    set_int(&c->audit_lines, o, "audit_lines");
    set_int(&c->sweep_every, o, "sweep_every");
    set_int(&c->archive_after, o, "archive_after");
    set_str(c->delegation, sizeof c->delegation, o, "delegation");
    set_str(c->verify, sizeof c->verify, o, "verify");

    // Named in the file, the kinds replace the built-in list rather than
    // adding to it: a board that drops a class means to be without it.
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

            // Absent, a kind takes every step: a class written by hand that
            // forgot to say is a class that gets the careful treatment.
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

/* ---- the files a person edits ------------------------------------------- */

// One directory, and nothing in it a person cannot open in an editor:
//
//   board/settings.md          the numbers, the delegation order, the check
//   board/roles/<who>.md       what runs each stage, and what it is told
//   board/kinds/<name>.md      a class of card: what it means, what it takes
//
// Everything is written out, whether or not it differs from the built-in
// default, because a file that is not there is a file nobody can edit.

#define BOARD_DIR "board"

static int board_path(char *out, size_t size, const char *leaf, const char *name)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, leaf))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s.md", dir, name) < size;
}

// "worktree, review, merge" -> a mask. An empty list is a kind that takes no
// step at all, which is not the same as one that did not say.
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
    c->usage_ceiling = mdcfg_int(&m, "usage ceiling", c->usage_ceiling);
    c->reset_hold = mdcfg_int(&m, "reset hold", c->reset_hold);
    c->audit_files = mdcfg_int(&m, "audit files", c->audit_files);
    c->audit_lines = mdcfg_int(&m, "audit lines", c->audit_lines);
    c->sweep_every = mdcfg_int(&m, "sweep every", c->sweep_every);
    c->archive_after = mdcfg_int(&m, "archive after", c->archive_after);

    const char *chain = mdcfg_get(&m, "delegation");
    if (*chain) {
        // Written with spaces because a person wrote it; stored without.
        size_t at = 0;
        for (const char *p = chain; *p && at + 1 < sizeof c->delegation; p++)
            if (*p != ' ')
                c->delegation[at++] = *p;
        c->delegation[at] = '\0';
    }
    snprintf(c->verify, sizeof c->verify, "%s", mdcfg_get(&m, "check"));
    mdcfg_free(&m);
}

static void read_roles(struct board_cfg *c)
{
    for (int i = 0; i < BOARD_WHO; i++) {
        char path[4300];
        if (!board_path(path, sizeof path, BOARD_DIR "/roles", WHO_NAMES[i]))
            continue;

        struct mdcfg m;
        if (!mdcfg_load(&m, path))
            continue;

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

    // The files are the list: a kind whose file was deleted is a kind the
    // board no longer has.
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

    char nums[7][32], chain[256];
    snprintf(nums[0], sizeof nums[0], "%d", c->workers);
    snprintf(nums[1], sizeof nums[1], "%d", c->usage_ceiling);
    snprintf(nums[2], sizeof nums[2], "%d", c->reset_hold);
    snprintf(nums[3], sizeof nums[3], "%d", c->audit_files);
    snprintf(nums[4], sizeof nums[4], "%d", c->audit_lines);
    snprintf(nums[5], sizeof nums[5], "%d", c->sweep_every);
    snprintf(nums[6], sizeof nums[6], "%d", c->archive_after);

    size_t at = 0;
    chain[0] = '\0';
    for (const char *p = c->delegation; *p && at + 2 < sizeof chain; p++) {
        chain[at++] = *p;
        if (*p == ',')
            chain[at++] = ' ';
    }
    chain[at] = '\0';

    const char *keys[] = {"workers", "usage ceiling", "reset hold", "audit files",
                          "audit lines", "sweep every", "archive after",
                          "delegation", "check"};
    const char *vals[] = {nums[0], nums[1], nums[2], nums[3], nums[4], nums[5],
                          nums[6], chain, c->verify};

    return mdcfg_write(path, keys, vals, 9,
        "How many workers may run at once, the percent of quota above which\n"
        "nothing starts, and how close to a reset is worth waiting for rather\n"
        "than handing on. A diff over either audit threshold is read before it\n"
        "lands; zero turns that half off. A sweep comes due every so many cards\n"
        "landed in a repo. Done cards leave the board after so many days.\n"
        "\n"
        "delegation is who takes over when a backend runs out, in order.\n"
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
        const char *keys[] = {"backend", "model", "effort"};
        const char *vals[] = {c->who[i].backend, c->who[i].model, c->who[i].effort};
        if (!mdcfg_write(path, keys, vals, 3, c->who[i].prompt))
            ok = 0;
    }
    return ok;
}

static int write_kinds(const struct board_cfg *c)
{
    char dir[4096];
    if (!mdcfg_dir(dir, sizeof dir, BOARD_DIR "/kinds"))
        return 0;

    // A kind edited into a different name leaves its old file behind, and a
    // deleted one leaves all of it, so what is no longer configured goes.
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
    if (!write_kinds(c))
        ok = 0;
    return ok;
}

static struct board_cfg cache;
static int              loaded;

static void load(void)
{
    if (loaded)
        return;
    loaded = 1;
    defaults(&cache);

    // A board.json from before the files existed is read once, so what was
    // configured then carries over, and then set aside.
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
    read_roles(&cache);
    read_kinds(&cache);

    // Nothing there to read means nothing there to edit, so the defaults are
    // written out the first time rather than waiting for a change.
    char seed[4300];
    if (board_path(seed, sizeof seed, BOARD_DIR, "settings") && access(seed, F_OK))
        write_out(&cache);
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

    // The width the names line up to, so the list reads as a table rather
    // than as a paragraph the model has to parse.
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
    return &cache.who[who];
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
    cache.usage_ceiling = c->usage_ceiling;
    cache.reset_hold = c->reset_hold;
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

    snprintf(cache.delegation, sizeof cache.delegation, "%s", c->delegation);
    snprintf(cache.verify, sizeof cache.verify, "%s", c->verify);
    for (int i = 0; i < BOARD_WHO; i++) {
        snprintf(cache.who[i].backend, sizeof cache.who[i].backend, "%s", c->who[i].backend);
        snprintf(cache.who[i].model, sizeof cache.who[i].model, "%s", c->who[i].model);
        snprintf(cache.who[i].effort, sizeof cache.who[i].effort, "%s", c->who[i].effort);
    }
    return write_out(&cache);
}
