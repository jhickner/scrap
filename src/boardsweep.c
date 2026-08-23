#include "boardsweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "boardcfg.h"
#include "child.h"
#include "replyjson.h"
#include "sessionfork.h"
#include "text.h"
#include "vendor/cJSON.h"

#define SWEEP_KEY "sweep:"
#define SWEEPS_MAX 4

// A repo is named by a path too long to be a key, so it is named by a number
// taken from the path, and the path is kept beside it.
static struct {
    char key[CHILD_KEY_MAX];
    char cwd[4096];
} going[SWEEPS_MAX];

static void sweep_key(const char *cwd, char *out, size_t size)
{
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)cwd; *p; p++)
        h = h * 33 + *p;
    snprintf(out, size, SWEEP_KEY "%08lx", h & 0xffffffffUL);
}

int boardsweep_running(const char *cwd)
{
    char key[CHILD_KEY_MAX];
    sweep_key(cwd, key, sizeof key);
    return child_running(key);
}

/* ---- when one is due ---------------------------------------------------- */

static const char *counts_path(void)
{
    static char p[4200];
    if (!p[0] && !path_config_file(p, sizeof p, "board-sweep.json"))
        snprintf(p, sizeof p, "/tmp/board-sweep.json");
    return p;
}

static cJSON *counts_load(void)
{
    char *text = text_slurp(counts_path(), 1u << 16, NULL);
    if (!text)
        return cJSON_CreateObject();
    cJSON *o = cJSON_Parse(text);
    free(text);
    return o ? o : cJSON_CreateObject();
}

static void counts_save(cJSON *o)
{
    char *text = cJSON_PrintUnformatted(o);
    if (!text)
        return;
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", counts_path());
    FILE *f = fopen(tmp, "wb");
    int   ok = f != NULL;
    if (ok && fprintf(f, "%s\n", text) < 0)
        ok = 0;
    if (f && fclose(f) != 0)
        ok = 0;
    free(text);
    if (!ok || rename(tmp, counts_path()) != 0)
        unlink(tmp);
}

static int count_of(const char *cwd)
{
    cJSON *o = counts_load();
    cJSON *e = cJSON_GetObjectItem(o, cwd);
    int    n = e && cJSON_IsNumber(e) ? (int)e->valuedouble : 0;
    cJSON_Delete(o);
    return n;
}

static void count_set(const char *cwd, int n)
{
    cJSON *o = counts_load();
    cJSON_DeleteItemFromObject(o, cwd);
    cJSON_AddNumberToObject(o, cwd, n);
    counts_save(o);
    cJSON_Delete(o);
}

int boardsweep_landed(const char *cwd)
{
    const struct board_cfg *cfg = boardcfg();
    if (!cwd || !*cwd || cfg->sweep_every <= 0)
        return 0;

    int n = count_of(cwd) + 1;
    count_set(cwd, n);
    return n >= cfg->sweep_every;
}

/* ---- what it is asked --------------------------------------------------- */

// What the audits have already said about this repo. A sweep that reads them
// is looking at evidence rather than starting from nothing, and duplication is
// exactly what an audit tends to catch one card at a time.
static char *findings_for(const char *cwd)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    size_t cap = 4096, len = 0;
    char  *out = malloc(cap);
    if (!out) {
        board_free(cards, n);
        return NULL;
    }
    out[0] = '\0';

    for (int i = 0; i < n; i++) {
        if (strcmp(cards[i].cwd, cwd))
            continue;
        for (int j = 0; j < cards[i].log_n; j++) {
            if (strcmp(cards[i].log[j].who, "audit") || !cards[i].log[j].text)
                continue;
            size_t add = strlen(cards[i].log[j].text) + 4;
            if (len + add >= cap)
                break;
            len += (size_t)snprintf(out + len, cap - len, "- %s\n",
                                    cards[i].log[j].text);
        }
    }
    board_free(cards, n);
    return out;
}

static char *build_prompt(const char *cwd)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_SWEEP);
    const char                 *head = p->prompt ? p->prompt : "";
    char                       *found = findings_for(cwd);

    size_t need = strlen(head) + (found ? strlen(found) : 0) + 256;
    char  *out = malloc(need);
    if (!out) {
        free(found);
        return NULL;
    }
    if (found && *found)
        snprintf(out, need,
                 "%s\n\nAudits of work already landed here said:\n\n%s\n"
                 "Those are a starting point, not the whole of it.\n", head, found);
    else
        snprintf(out, need, "%s\n", head);
    free(found);
    return out;
}

int boardsweep_start(const char *cwd)
{
    if (!cwd || !*cwd)
        return 0;

    char key[CHILD_KEY_MAX];
    sweep_key(cwd, key, sizeof key);
    if (child_running(key))
        return 0;

    int at = -1;
    for (int i = 0; i < SWEEPS_MAX && at < 0; i++)
        if (!going[i].key[0] || !strcmp(going[i].key, key))
            at = i;
    if (at < 0)
        return 0;

    char *prompt = build_prompt(cwd);
    if (!prompt)
        return 0;

    const struct board_profile *p = boardcfg_for(BOARD_WHO_SWEEP);

    char *argv[16];
    int   n = 0;
    argv[n++] = (char *)sessionfork_program();
    argv[n++] = (char *)"-b";
    argv[n++] = (char *)(p->backend[0] ? p->backend : "claude");
    if (p->model[0] && strcmp(p->model, "default")) {
        argv[n++] = (char *)"-m";
        argv[n++] = (char *)p->model;
    }
    if (p->effort[0] && strcmp(p->effort, "default")) {
        argv[n++] = (char *)"-e";
        argv[n++] = (char *)p->effort;
    }
    argv[n++] = prompt;
    argv[n] = NULL;

    int ok = child_start(key, argv, cwd);
    free(prompt);
    if (ok) {
        snprintf(going[at].key, sizeof going[at].key, "%s", key);
        snprintf(going[at].cwd, sizeof going[at].cwd, "%s", cwd);
        // The count starts again whether or not it finds anything: the point
        // is a look every so often, not a look until it finds something.
        count_set(cwd, 0);
    }
    return ok;
}

int boardsweep_pump(void)
{
    const struct board_cfg *cfg = boardcfg();
    if (cfg->sweep_every <= 0)
        return 0;

    cJSON *o = counts_load();
    char   due[4096] = "";
    for (cJSON *e = o->child; e; e = e->next) {
        if (!cJSON_IsNumber(e) || !e->string)
            continue;
        if ((int)e->valuedouble < cfg->sweep_every)
            continue;
        snprintf(due, sizeof due, "%s", e->string);
        break;
    }
    cJSON_Delete(o);

    return due[0] ? boardsweep_start(due) : 0;
}

/* ---- what it found ------------------------------------------------------ */

int boardsweep_take(const char *key, const char *reply)
{
    if (!key || strncmp(key, SWEEP_KEY, strlen(SWEEP_KEY)))
        return 0;

    char cwd[4096] = "";
    for (int i = 0; i < SWEEPS_MAX; i++)
        if (!strcmp(going[i].key, key)) {
            snprintf(cwd, sizeof cwd, "%s", going[i].cwd);
            memset(&going[i], 0, sizeof going[i]);
            break;
        }
    if (!cwd[0])
        return 1;

    cJSON *o = replyjson_parse(reply);
    if (!o)
        return 1;

    const cJSON *cards = cJSON_GetObjectItem(o, "cards"), *e = NULL;
    cJSON_ArrayForEach(e, cards) {
        const char *text = cJSON_GetStringValue((cJSON *)e);
        if (!text || !*text)
            continue;
        // Filed as an ordinary card, so triage sorts it like anything else and
        // nothing here has to decide what it is.
        char id[BOARD_ID_MAX];
        if (board_add(text, cwd, id))
            board_note(id, "sweep", "found while looking over the repo");
    }
    cJSON_Delete(o);
    return 1;
}
