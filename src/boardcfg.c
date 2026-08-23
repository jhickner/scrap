#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "vendor/cJSON.h"

#define CFG_MAX_BYTES (1u << 20)

static const char *const WHO_NAMES[BOARD_WHO] = {"triage", "worker", "audit",
                                                 "sweep"};

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
    "  {\"kind\":\"...\",\"title\":\"...\",\"spec\":\"...\",\"cwd\":\"...\",\"priority\":0,\"confidence\":0.0,\"question\":\"\"}\n"
    "\n"
    "kind is one of: todo, data, reference, feature, bug, chore.\n"
    "  todo       something the person means to do, off the computer or on it\n"
    "  data       a measurement, a number, a result, a scrap worth keeping\n"
    "  reference  a link, a name, a fact, something to look up again later\n"
    "  feature    something that should exist and does not\n"
    "  bug        something that exists and is wrong\n"
    "  chore      upkeep: a rename, a bump, a cleanup\n"
    "\n"
    "title is a short name for the card, under 60 characters.\n"
    "spec is what the card asks for, in a few sentences. Do not invent\n"
    "  requirements the card does not imply.\n"
    "cwd is the absolute path of the repo it belongs to.\n"
    "priority is 0 to 3, 0 being ordinary.\n"
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

// The prompt each job has when nothing has been written over it. Kept
// reachable so a saved copy identical to it can be recognised and left out of
// the file -- otherwise opening the config screen once would freeze the
// defaults as they were that day, and a better one written later would never
// reach anybody.
static const char *default_prompt(enum board_who who)
{
    switch (who) {
    case BOARD_WHO_WORKER: return WORKER_PROMPT;
    case BOARD_WHO_AUDIT:  return AUDIT_PROMPT;
    case BOARD_WHO_SWEEP:  return SWEEP_PROMPT;
    default:               return TRIAGE_PROMPT;
    }
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

    for (int i = 0; i < BOARD_WHO; i++)
        snprintf(c->who[i].backend, sizeof c->who[i].backend, "claude");

    // Triage is a classifier: it wants the cheap model and little thinking.
    snprintf(c->who[BOARD_WHO_TRIAGE].model, sizeof c->who[BOARD_WHO_TRIAGE].model, "haiku");
    snprintf(c->who[BOARD_WHO_TRIAGE].effort, sizeof c->who[BOARD_WHO_TRIAGE].effort, "low");
    snprintf(c->who[BOARD_WHO_AUDIT].effort, sizeof c->who[BOARD_WHO_AUDIT].effort, "high");

    c->who[BOARD_WHO_TRIAGE].prompt = dup_or_null(TRIAGE_PROMPT);
    c->who[BOARD_WHO_WORKER].prompt = dup_or_null(WORKER_PROMPT);
    c->who[BOARD_WHO_AUDIT].prompt = dup_or_null(AUDIT_PROMPT);
    c->who[BOARD_WHO_SWEEP].prompt = dup_or_null(SWEEP_PROMPT);
}

static const char *path(void)
{
    static char p[4200];
    if (!p[0] && !path_config_file(p, sizeof p, "board.json"))
        snprintf(p, sizeof p, "/tmp/board.json");
    return p;
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

// Read over the defaults rather than in place of them, so a file written by an
// older build, or half-edited by hand, still yields a working board.
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

static struct board_cfg cache;
static int              loaded;

static void load(void)
{
    if (loaded)
        return;
    loaded = 1;
    defaults(&cache);

    size_t len = 0;
    char  *text = text_slurp(path(), CFG_MAX_BYTES, &len);
    if (!text)
        return;
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (o) {
        overlay(&cache, o);
        cJSON_Delete(o);
    }
}

const struct board_cfg *boardcfg(void)
{
    load();
    return &cache;
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
    return c;
}

void boardcfg_free(struct board_cfg *c)
{
    if (!c)
        return;
    for (int i = 0; i < BOARD_WHO; i++)
        free(c->who[i].prompt);
    free(c);
}

static int write_out(const struct board_cfg *c)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return 0;

    cJSON_AddNumberToObject(o, "workers", c->workers);
    cJSON_AddNumberToObject(o, "usage_ceiling", c->usage_ceiling);
    cJSON_AddNumberToObject(o, "reset_hold", c->reset_hold);
    cJSON_AddNumberToObject(o, "audit_files", c->audit_files);
    cJSON_AddNumberToObject(o, "audit_lines", c->audit_lines);
    cJSON_AddNumberToObject(o, "sweep_every", c->sweep_every);
    cJSON_AddNumberToObject(o, "archive_after", c->archive_after);
    cJSON_AddStringToObject(o, "delegation", c->delegation);
    cJSON_AddStringToObject(o, "verify", c->verify);

    cJSON *who = cJSON_AddObjectToObject(o, "who");
    if (!who) {
        cJSON_Delete(o);
        return 0;
    }
    for (int i = 0; i < BOARD_WHO; i++) {
        cJSON *p = cJSON_AddObjectToObject(who, WHO_NAMES[i]);
        if (!p)
            break;
        cJSON_AddStringToObject(p, "backend", c->who[i].backend);
        cJSON_AddStringToObject(p, "model", c->who[i].model);
        cJSON_AddStringToObject(p, "effort", c->who[i].effort);
        // Only a prompt somebody has actually changed is written down.
        const char *prompt = c->who[i].prompt;
        const char *stock = default_prompt((enum board_who)i);
        if (prompt && strcmp(prompt, stock))
            cJSON_AddStringToObject(p, "prompt", prompt);
    }

    char *text = cJSON_Print(o);
    cJSON_Delete(o);
    if (!text)
        return 0;

    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path());
    FILE *f = fopen(tmp, "wb");
    int   ok = f != NULL;
    if (ok && fprintf(f, "%s\n", text) < 0)
        ok = 0;
    if (f && fclose(f) != 0)
        ok = 0;
    free(text);

    if (!ok || rename(tmp, path()) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
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
    snprintf(cache.delegation, sizeof cache.delegation, "%s", c->delegation);
    snprintf(cache.verify, sizeof cache.verify, "%s", c->verify);
    for (int i = 0; i < BOARD_WHO; i++) {
        snprintf(cache.who[i].backend, sizeof cache.who[i].backend, "%s", c->who[i].backend);
        snprintf(cache.who[i].model, sizeof cache.who[i].model, "%s", c->who[i].model);
        snprintf(cache.who[i].effort, sizeof cache.who[i].effort, "%s", c->who[i].effort);
    }
    return write_out(&cache);
}
