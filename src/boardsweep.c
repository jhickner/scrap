#include "boardsweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "gitcmd.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

#define SWEEP_MARK "swept"

#define SWEEP_PROPOSALS_MAX 16

struct proposal {
    char *text;
    char  repo[4096];
};

static struct proposal proposals[SWEEP_PROPOSALS_MAX];
static int             proposals_n;

static void sweep_root(const char *cwd, char *out, size_t size)
{
    if (!cwd || !*cwd || !gitcmd_root(cwd, out, size))
        snprintf(out, size, "%s", cwd ? cwd : "");
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

int boardsweep_due(const struct board_card *cards, int n, char *id, size_t idsize,
                   char *root, size_t rootsize)
{
    const struct board_cfg *cfg = boardcfg();
    if (cfg->sweep_every <= 0)
        return 0;

    for (int i = 0; i < n; i++) {
        if (!unswept(&cards[i]) || !newest_landed(cards, i) ||
            landed_since(cards, n, cards[i].cwd) < cfg->sweep_every)
            continue;
        snprintf(id, idsize, "%s", cards[i].id);
        sweep_root(cards[i].cwd, root, rootsize);
        return 1;
    }
    return 0;
}

char *boardsweep_prompt(const struct board_card *cards, int n, const char *cwd)
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

static void propose(const char *text, const char *repo)
{
    if (proposals_n >= SWEEP_PROPOSALS_MAX)
        return;
    char *copy = strdup(text);
    if (!copy)
        return;
    proposals[proposals_n].text = copy;
    snprintf(proposals[proposals_n].repo, sizeof proposals[proposals_n].repo,
             "%s", repo);
    proposals_n++;
}

int boardsweep_finished(const char *id, const char *reply)
{
    if (!id || !*id)
        return 0;

    boardlog_turn(id, "sweep", NULL, reply);

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
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
    int          was = proposals_n;
    cJSON_ArrayForEach(e, raised) {
        const char *text = cJSON_GetStringValue((cJSON *)e);
        if (text && *text)
            propose(text, root);
    }
    cJSON_Delete(o);

    char said[64];
    snprintf(said, sizeof said, "proposed %d card%s", proposals_n - was,
             proposals_n - was == 1 ? "" : "s");
    board_note(id, "sweep", said);
    return 1;
}

int boardsweep_proposed(void)
{
    return proposals_n;
}

const char *boardsweep_proposal(int at)
{
    if (at < 0 || at >= proposals_n)
        return NULL;
    return proposals[at].text;
}

const char *boardsweep_proposal_repo(int at)
{
    if (at < 0 || at >= proposals_n)
        return NULL;
    return proposals[at].repo;
}

void boardsweep_drop(int at)
{
    if (at < 0 || at >= proposals_n)
        return;
    free(proposals[at].text);
    for (int i = at; i < proposals_n - 1; i++)
        proposals[i] = proposals[i + 1];
    proposals_n--;
}

void boardsweep_drop_all(void)
{
    while (proposals_n)
        boardsweep_drop(proposals_n - 1);
}

int boardsweep_accept(int at, const char *text)
{
    if (at < 0 || at >= proposals_n)
        return 0;
    if (!text || !*text)
        text = proposals[at].text;

    char id[BOARD_ID_MAX];
    int  ok = board_add(text, proposals[at].repo, id);
    if (ok)
        board_note(id, "sweep", "raised by a sweep");
    boardsweep_drop(at);
    return ok;
}
