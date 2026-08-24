#include "boardstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardflow.h"
#include "boardlog.h"

const char *boardstep_before(const struct board_card *c)
{
    if (!c)
        return NULL;
    for (int at = boardcfg_kind_step_at(c->kind, c->step) - 1; at >= 0; at--) {
        const struct board_role *p = boardcfg_for_step(boardcfg_kind_step(c->kind, at));
        const char              *said = p ? board_said(c, p->job) : NULL;
        if (said)
            return said;
    }
    return NULL;
}

char *boardstep_prompt(const struct board_card *c, const struct board_role *p)
{
    if (!c || !p)
        return NULL;

    const char *head = p->prompt ? p->prompt : "";
    const char *body = c->body && *c->body ? c->body : c->title;
    const char *said = boardstep_before(c);

    size_t need = strlen(head) + strlen(body) + strlen(c->title) +
                  (said ? strlen(said) : 0) + sizeof c->base + 256;
    char *out = malloc(need);
    if (!out)
        return NULL;

    int at = snprintf(out, need,
                      "%s\n\nThe branch came off %s. The card it was built "
                      "for:\n\n# %s\n\n%s\n",
                      head, c->base, c->title, body);
    if (said)
        snprintf(out + at, need - (size_t)at,
                 "\nThe step before this one said:\n\n%s\n", said);
    return out;
}

int boardstep_finished(const struct board_card *c, const struct board_role *p,
                       const char *reply)
{
    if (!c || !p)
        return 0;

    boardlog_turn(c->id, p->job, NULL, reply);
    if (reply && *reply)
        board_note(c->id, p->job, reply);

    if (!board_at(c, p->step))
        return 0;

    if (p->fail_marker[0] && reply && strstr(reply, p->fail_marker))
        return board_move_back(c->id, boardflow_fail(c), p->job, NULL);
    return board_move_to(c->id, boardflow_next(c, p->step, 0), p->job, NULL);
}
