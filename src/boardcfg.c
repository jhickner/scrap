#include "boardcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "vendor/cJSON.h"

#define CFG_MAX_BYTES (1u << 20)

static const char *const WHO_NAMES[BOARD_WHO] = {"triage", "worker", "audit"};

const char *boardcfg_who_name(enum board_who who)
{
    if (who < 0 || who >= BOARD_WHO)
        return WHO_NAMES[BOARD_WHO_TRIAGE];
    return WHO_NAMES[who];
}

// Triage is told it may fail. A classifier given only a list of kinds will
// always pick one, and a confident wrong kind costs more than an admitted
// unknown: the first sends a worker somewhere, the second asks a question.
static const char TRIAGE_PROMPT[] =
    "You are sorting one card on a work board. Read it and answer with JSON "
    "only, no prose and no code fence.\n"
    "\n"
    "  {\"kind\":\"...\",\"title\":\"...\",\"spec\":\"...\",\"cwd\":\"...\","
    "\"priority\":0,\"confidence\":0.0,\"question\":\"\"}\n"
    "\n"
    "kind is one of: todo, data, reference, feature, bug, chore.\n"
    "  todo/data/reference are notes to file, and want no code written.\n"
    "  feature/bug/chore are work for an agent in a repo.\n"
    "title is a short name for the card, under 60 characters.\n"
    "spec is what a competent engineer would need to start, in a few "
    "sentences. Do not invent requirements the card does not imply.\n"
    "cwd is the absolute path of the repo it belongs to, or \"\" if unclear.\n"
    "priority is 0 to 3, 0 being ordinary.\n"
    "confidence is 0.0 to 1.0: how sure you are of kind and cwd.\n"
    "question is what you would need to ask to be sure, or \"\".\n"
    "\n"
    "If the card is ambiguous, say so: set confidence below 0.5 and put the "
    "question you would ask in question. Guessing is worse than asking. Do "
    "not ask about anything you could reasonably decide yourself.";

static const char WORKER_PROMPT[] =
    "You are working one card from a board, alone, in a worktree of your own. "
    "Reach a state someone else can test, then stop and say how to test it. "
    "Do not merge, do not commit to the main branch, and do not start work "
    "the card does not ask for.";

static const char AUDIT_PROMPT[] =
    "Review the diff on this branch for problems worth a second pass: "
    "duplicated mechanisms that should be one, architecture that fights the "
    "code around it, memory handling, and anything with a security cost. "
    "Report only what you would stop a merge for.";

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
    snprintf(c->delegation, sizeof c->delegation, "claude,codex,grok");

    for (int i = 0; i < BOARD_WHO; i++)
        snprintf(c->who[i].backend, sizeof c->who[i].backend, "claude");

    // Triage is a classifier: it wants the cheap model and little thinking.
    snprintf(c->who[BOARD_WHO_TRIAGE].model, sizeof c->who[BOARD_WHO_TRIAGE].model, "haiku");
    snprintf(c->who[BOARD_WHO_TRIAGE].effort, sizeof c->who[BOARD_WHO_TRIAGE].effort, "low");
    snprintf(c->who[BOARD_WHO_AUDIT].effort, sizeof c->who[BOARD_WHO_AUDIT].effort, "high");

    c->who[BOARD_WHO_TRIAGE].prompt = dup_or_null(TRIAGE_PROMPT);
    c->who[BOARD_WHO_WORKER].prompt = dup_or_null(WORKER_PROMPT);
    c->who[BOARD_WHO_AUDIT].prompt = dup_or_null(AUDIT_PROMPT);
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
    set_str(c->delegation, sizeof c->delegation, o, "delegation");

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
    cJSON_AddStringToObject(o, "delegation", c->delegation);

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
        cJSON_AddStringToObject(p, "prompt", c->who[i].prompt ? c->who[i].prompt : "");
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
    snprintf(cache.delegation, sizeof cache.delegation, "%s", c->delegation);
    for (int i = 0; i < BOARD_WHO; i++) {
        snprintf(cache.who[i].backend, sizeof cache.who[i].backend, "%s", c->who[i].backend);
        snprintf(cache.who[i].model, sizeof cache.who[i].model, "%s", c->who[i].model);
        snprintf(cache.who[i].effort, sizeof cache.who[i].effort, "%s", c->who[i].effort);
    }
    return write_out(&cache);
}
