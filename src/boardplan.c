#include "boardplan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"

#define PLAN_KIND "plan"

int boardplan_is(const struct board_card *c)
{
    return c && boardplan_named(c->kind);
}

int boardplan_named(const char *kind)
{
    return kind && !strcmp(kind, PLAN_KIND);
}

static int bookkeeping(const char *who)
{
    return !strcmp(who, "board") || !strcmp(who, "triage") ||
           !strcmp(who, "plan") || !strcmp(who, "sweep");
}

/* everything said about the card from the first worker turn on: the plan and
   whatever was discussed after it */
char *boardplan_discussion(const struct board_card *c)
{
    if (!c)
        return NULL;

    const struct board_role *mine = boardcfg_worker();
    const char              *job = mine && mine->job[0] ? mine->job : "worker";

    int from = -1;
    for (int i = 0; i < c->log_n && from < 0; i++)
        if (!strcmp(c->log[i].who, job) && c->log[i].text && *c->log[i].text)
            from = i;
    if (from < 0)
        for (int i = 0; i < c->log_n && from < 0; i++)
            if (!bookkeeping(c->log[i].who) && c->log[i].text &&
                *c->log[i].text)
                from = i;
    if (from < 0)
        return NULL;

    char  *out = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&out, &len);
    if (!f)
        return NULL;

    for (int i = from; i < c->log_n; i++) {
        const char *who = c->log[i].who, *text = c->log[i].text;
        if (!text || !*text || bookkeeping(who))
            continue;
        fprintf(f, "%s%s said:\n\n%s\n", len ? "\n" : "", who, text);
    }
    fclose(f);

    if (len)
        return out;
    free(out);
    return NULL;
}

static char *filed_body(const struct board_card *c, const char *plan)
{
    const char *spec = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(spec) + strlen(plan) + 32;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need, "## Spec\n\n%s\n\n## Plan\n\n%s", spec, plan);
    return out;
}

int boardplan_approve(const struct board_card *c)
{
    if (!boardplan_is(c))
        return 0;

    char *plan = boardplan_discussion(c);
    if (!plan)
        return 0;

    char *body = filed_body(c, plan);
    free(plan);
    if (!body)
        return 0;

    char id[BOARD_ID_MAX];
    int  added = board_add(body, c->cwd, id);
    free(body);
    if (!added)
        return 0;

    const struct board_kind *k = boardcfg_kind(c->kind);
    const char              *next = k ? k->next_kind : "";

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *made = board_find(v, n, id);
    if (made) {
        struct board_card edited = *made;
        snprintf(edited.title, sizeof edited.title, "%s", c->title);
        if (*next) {
            snprintf(edited.kind, sizeof edited.kind, "%s", next);
            edited.priority = boardcfg_priority(next);
            edited.col = BOARD_BACKLOG;
        }
        board_update(&edited);
    }
    board_free(v, n);

    board_note(id, "plan", "raised by a plan");

    char said[64];
    snprintf(said, sizeof said, "filed card %s", id);
    board_note(c->id, "you", said);
    return board_move(c->id, BOARD_DONE, NULL, "you", NULL);
}
