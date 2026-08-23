#include "boardsweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "child.h"
#include "gitcmd.h"
#include "replyjson.h"
#include "sessionfork.h"
#include "vendor/cJSON.h"

#define SWEEP_KEY  "sweep:"
#define SWEEP_MARK "swept"

static void sweep_root(const char *cwd, char *out, size_t size)
{
    if (!cwd || !*cwd || !gitcmd_root(cwd, out, size))
        snprintf(out, size, "%s", cwd ? cwd : "");
}

static void sweep_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, SWEEP_KEY "%s", id);
}

static int swept(const struct board_card *c)
{
    for (int i = 0; i < c->log_n; i++)
        if (!strcmp(c->log[i].who, "sweep") && c->log[i].text &&
            !strcmp(c->log[i].text, SWEEP_MARK))
            return 1;
    return 0;
}

static int unswept(const struct board_card *c)
{
    return c->col == BOARD_DONE && c->cwd[0] && !swept(c);
}

static int landed_since(const struct board_card *cards, int n, const char *cwd)
{
    int count = 0;
    for (int i = 0; i < n; i++)
        if (unswept(&cards[i]) && !strcmp(cards[i].cwd, cwd))
            count++;
    return count;
}

static void mark_swept(const char *cwd)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    for (int i = 0; i < n; i++)
        if (unswept(&cards[i]) && !strcmp(cards[i].cwd, cwd))
            board_note(cards[i].id, "sweep", SWEEP_MARK);
    board_free(cards, n);
}

static int newest_landed(const struct board_card *cards, int at)
{
    for (int i = 0; i < at; i++)
        if (unswept(&cards[i]) && !strcmp(cards[i].cwd, cards[at].cwd))
            return 0;
    return 1;
}

static char *findings_for(const struct board_card *cards, int n, const char *cwd)
{
    size_t cap = 4096, len = 0;
    char  *out = malloc(cap);
    if (!out)
        return NULL;
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
    return out;
}

static char *build_prompt(const struct board_card *cards, int n, const char *cwd)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_SWEEP);
    const char                 *head = p->prompt ? p->prompt : "";
    char                       *found = findings_for(cards, n, cwd);

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

static int sweep_start(const struct board_card *c, const struct board_card *cards, int n)
{
    char key[CHILD_KEY_MAX];
    sweep_key(c->id, key, sizeof key);
    if (child_running(key))
        return 0;

    char *prompt = build_prompt(cards, n, c->cwd);
    if (!prompt)
        return 0;

    const struct board_profile *p = boardcfg_for(BOARD_WHO_SWEEP);

    char *argv[16];
    int   k = 0;
    argv[k++] = (char *)sessionfork_program();
    argv[k++] = (char *)"-b";
    argv[k++] = (char *)(p->backend[0] ? p->backend : "claude");
    if (p->model[0] && strcmp(p->model, "default")) {
        argv[k++] = (char *)"-m";
        argv[k++] = (char *)p->model;
    }
    if (p->effort[0] && strcmp(p->effort, "default")) {
        argv[k++] = (char *)"-e";
        argv[k++] = (char *)p->effort;
    }
    argv[k++] = prompt;
    argv[k] = NULL;

    char root[4096];
    sweep_root(c->cwd, root, sizeof root);

    int ok = child_start(key, argv, root);
    if (ok)
        boardlog_turn(c->id, "sweep", prompt, NULL);
    free(prompt);
    return ok;
}

int boardsweep_pump(void)
{
    const struct board_cfg *cfg = boardcfg();
    if (cfg->sweep_every <= 0)
        return 0;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++) {
        if (!unswept(&cards[i]) || !newest_landed(cards, i) ||
            landed_since(cards, n, cards[i].cwd) < cfg->sweep_every)
            continue;
        started = sweep_start(&cards[i], cards, n);
    }

    board_free(cards, n);
    return started;
}

int boardsweep_take(const char *key, const char *reply)
{
    size_t mark = strlen(SWEEP_KEY);
    if (!key || strncmp(key, SWEEP_KEY, mark))
        return 0;

    const char *host = key + mark;
    boardlog_turn(host, "sweep", NULL, reply);

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, host);
    char               cwd[4096] = "";
    if (c)
        snprintf(cwd, sizeof cwd, "%s", c->cwd);
    board_free(cards, n);
    if (!cwd[0])
        return 1;

    mark_swept(cwd);

    char root[4096];
    sweep_root(cwd, root, sizeof root);

    cJSON *o = replyjson_parse(reply);
    if (!o)
        return 1;

    const cJSON *raised = cJSON_GetObjectItem(o, "cards"), *e = NULL;
    cJSON_ArrayForEach(e, raised) {
        const char *text = cJSON_GetStringValue((cJSON *)e);
        if (!text || !*text)
            continue;

        char id[BOARD_ID_MAX];
        if (board_add(text, root, id))
            board_note(id, "sweep", "raised by a sweep");
    }
    cJSON_Delete(o);
    return 1;
}
