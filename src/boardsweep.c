#include "boardsweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "gitcmd.h"
#include "replyjson.h"
#include "text.h"
#include "vendor/cJSON.h"

#define SWEEP_KIND "sweep"
#define SWEEP_MARK "swept"

static void sweep_root(const char *cwd, char *out, size_t size)
{
    if (!cwd || !*cwd || !gitcmd_root(cwd, out, size))
        snprintf(out, size, "%s", cwd ? cwd : "");
}

int boardsweep_is(const struct board_card *c)
{
    return c && !strcmp(c->kind, SWEEP_KIND);
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
    return c->col == BOARD_DONE && c->cwd[0] && !boardsweep_is(c) && !swept(c);
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

/* an escape or a control byte means the note is a dumped error, not a finding */
static int readable(const char *text)
{
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == 0x1b || ch == 0x7f || (ch < ' ' && ch != '\n' && ch != '\t'))
            return 0;
    }
    return 1;
}

static char *findings_for(const struct board_card *cards, int n, const char *cwd)
{
    const struct board_role *p = boardcfg_for_job("audit");
    if (p && !p->fail_marker[0])
        p = NULL;

    size_t cap = 4096, len = 0;
    char  *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';

    for (int i = 0; i < n; i++) {
        if (strcmp(cards[i].cwd, cwd))
            continue;
        for (int j = 0; j < cards[i].log_n; j++) {
            const char *text = cards[i].log[j].text;
            if (!p || strcmp(cards[i].log[j].who, p->job) || !text ||
                !strstr(text, p->fail_marker) || !readable(text))
                continue;
            char one[1024];
            text_one_line(text, one, sizeof one);
            if (!one[0])
                continue;
            size_t add = strlen(one) + 4;
            if (len + add >= cap)
                break;
            len += (size_t)snprintf(out + len, cap - len, "- %s\n", one);
        }
    }
    return out;
}

int boardsweep_due(const struct board_card *cards, int n, char *cwd, size_t size)
{
    const struct board_cfg *cfg = boardcfg();
    if (cfg->sweep_every <= 0)
        return 0;

    for (int i = 0; i < n; i++) {
        if (!unswept(&cards[i]) ||
            landed_since(cards, n, cards[i].cwd) < cfg->sweep_every)
            continue;
        snprintf(cwd, size, "%s", cards[i].cwd);
        return 1;
    }
    return 0;
}

char *boardsweep_prompt(const struct board_card *cards, int n, const char *cwd)
{
    const struct board_role *p = boardcfg_for_job("sweep");
    const char                 *head = p && p->prompt ? p->prompt : "";
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

int boardsweep_open(const char *cwd, char *id, size_t size)
{
    char root[4096];
    sweep_root(cwd, root, sizeof root);

    char where[4096];
    path_home_relative(root, where, sizeof where);

    char text[4200];
    snprintf(text, sizeof text, "sweep of %s", where);

    char minted[BOARD_ID_MAX];
    if (!board_add(text, root, minted))
        return 0;

    mark_swept(cwd);

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, minted);
    int                ok = 0;
    if (c) {
        struct board_card edited = *c;
        edited.col = BOARD_STEP;
        snprintf(edited.step, sizeof edited.step, "%s", SWEEP_KIND);
        edited.body = NULL;
        snprintf(edited.kind, sizeof edited.kind, "%s", SWEEP_KIND);
        ok = board_update(&edited);
    }
    board_free(cards, n);

    if (!ok) {
        board_remove(minted);
        return 0;
    }
    snprintf(id, size, "%s", minted);
    return 1;
}

static const char *next_line(const char *at, char *out, size_t size)
{
    while (*at == '\n' || *at == ' ')
        at++;
    if (!*at)
        return NULL;

    const char *end = strchr(at, '\n');
    size_t      len = end ? (size_t)(end - at) : strlen(at);
    if (len >= size) {
        len = size - 1;
        while (len && ((unsigned char)at[len] & 0xC0) == 0x80)
            len--;
    }
    memcpy(out, at, len);
    out[len] = '\0';
    return end ? end + 1 : at + strlen(at);
}

int boardsweep_proposed(const struct board_card *c)
{
    if (!boardsweep_is(c) || !c->body)
        return 0;

    int         k = 0;
    char        line[BOARD_TITLE_MAX];
    const char *at = c->body;
    while ((at = next_line(at, line, sizeof line)))
        k++;
    return k;
}

int boardsweep_proposal(const struct board_card *c, int i, char *out, size_t size)
{
    if (!boardsweep_is(c) || !c->body || i < 0)
        return 0;

    const char *at = c->body;
    for (int k = 0; at; k++) {
        at = next_line(at, out, size);
        if (!at)
            return 0;
        if (k == i)
            return 1;
    }
    return 0;
}

int boardsweep_finished(const char *id, const char *reply)
{
    if (!id || !*id)
        return 0;

    boardlog_turn(id, "sweep", NULL, reply);

    size_t cap = 8192, len = 0;
    char  *body = malloc(cap);
    if (!body)
        return 0;
    body[0] = '\0';

    int    raised = 0;
    cJSON *o = replyjson_parse(reply);
    if (o) {
        const cJSON *cards = cJSON_GetObjectItem(o, "cards"), *e = NULL;
        cJSON_ArrayForEach(e, cards) {
            const char *text = cJSON_GetStringValue((cJSON *)e);
            if (!text || !*text || len + strlen(text) + 2 >= cap)
                continue;
            len += (size_t)snprintf(body + len, cap - len, "%s\n", text);
            raised++;
        }
        cJSON_Delete(o);
    }

    if (!raised) {
        free(body);
        board_note(id, "sweep", "nothing to raise");
        return board_move(id, BOARD_DONE, NULL, "sweep", NULL);
    }

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (c) {
        struct board_card edited = *c;
        edited.body = body;
        board_update(&edited);
    }
    board_free(v, n);
    free(body);

    char said[64];
    snprintf(said, sizeof said, "proposed %d card%s", raised, raised == 1 ? "" : "s");
    board_note(id, "sweep", said);

    n = board_load(&v);
    c = board_find(v, n, id);
    const char *next = c ? boardflow_next(c, c->step, 0) : NULL;
    int         moved = board_move_to(id, next, "sweep", NULL);
    board_free(v, n);
    return moved;
}

int boardsweep_approve(const struct board_card *c)
{
    if (!boardsweep_is(c))
        return 0;

    int         raised = 0;
    char        line[BOARD_TITLE_MAX];
    const char *at = c->body ? c->body : "";
    while ((at = next_line(at, line, sizeof line))) {
        char id[BOARD_ID_MAX];
        if (!board_add(line, c->cwd, id))
            continue;
        board_note(id, "sweep", "raised by a sweep");
        raised++;
    }

    char said[64];
    snprintf(said, sizeof said, "raised %d card%s", raised, raised == 1 ? "" : "s");
    board_note(c->id, "you", said);
    return board_move(c->id, BOARD_DONE, NULL, "you", NULL);
}

int boardsweep_reject(const struct board_card *c)
{
    if (!boardsweep_is(c))
        return 0;
    return board_move(c->id, BOARD_DONE, NULL, "you", "proposals dropped");
}
