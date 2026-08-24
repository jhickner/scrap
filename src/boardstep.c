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

char *boardstep_since(const struct board_card *c, const char *job)
{
    if (!c || !job || !*job)
        return NULL;

    int from = -1;
    for (int i = c->log_n - 1; i >= 0 && from < 0; i--)
        if (!strcmp(c->log[i].who, job))
            from = i + 1;
    if (from < 0)
        return NULL;

    char  *out = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&out, &len);
    if (!f)
        return NULL;

    for (int i = from; i < c->log_n; i++) {
        const char *who = c->log[i].who, *text = c->log[i].text;
        if (!text || !*text)
            continue;
        if (!strcmp(who, "board"))
            continue;
        fprintf(f, "%s%s said:\n\n%s\n", len ? "\n" : "", who, text);
    }
    fclose(f);

    if (len)
        return out;
    free(out);
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

    int failed = p->fail_marker[0] && reply && strstr(reply, p->fail_marker);

    if (reply && *reply)
        board_note(c->id, p->job, reply);

    if (!board_at(c, p->step))
        return 0;

    if (failed)
        return board_move_back(c->id, boardflow_fail(c), p->job, NULL);
    return board_move_to(c->id, boardflow_next(c, p->step, 0), p->job, NULL);
}
