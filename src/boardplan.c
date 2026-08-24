#include "boardplan.h"

#include <stdio.h>
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

/* the plan a card wrote before it was called a plan: a kind can be corrected
   after the worker has answered, and then the answer is the plan. */
const char *boardplan_said(const struct board_card *c)
{
    return board_said(c, "worker");
}

int boardplan_approve(const struct board_card *c)
{
    if (!boardplan_is(c) || !c->body || !*c->body)
        return 0;

    char id[BOARD_ID_MAX];
    if (!board_add(c->body, c->cwd, id))
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
