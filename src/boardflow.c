#include "boardflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boarddiff.h"

const struct board_role *boardflow_role(const struct board_card *c)
{
    if (!c || c->col != BOARD_STEP)
        return NULL;
    return boardcfg_for_step(c->step);
}

enum board_runs boardflow_runs(const struct board_card *c)
{
    const struct board_role *p = boardflow_role(c);
    return p ? p->runs : BOARD_RUNS_MODES;
}

const char *boardflow_next(const struct board_card *c, const char *after,
                           int force)
{
    if (!c)
        return NULL;

    int at = after && *after ? boardcfg_kind_step_at(c->kind, after) + 1 : 0;
    for (const char *step; (step = boardcfg_kind_step(c->kind, at)); at++) {
        const struct board_role *p = boardcfg_for_step(step);
        if (!p)
            continue;
        if (!force && !boarddiff_over(p, c))
            continue;
        return p->step;
    }
    return NULL;
}

const char *boardflow_start(void)
{
    const struct board_role *p = boardcfg_worker();
    return p ? p->step : NULL;
}

const char *boardflow_after_turn(const struct board_card *c)
{
    const char *next = boardflow_next(c, c->step, 0);
    char       *say = boardflow_approval(c);
    if (say) {
        free(say);
        return next;
    }

    const struct board_kind *k = boardcfg_kind(c->kind);
    if (!k || !k->approval_prompt || !*k->approval_prompt)
        return next;

    while (next) {
        const struct board_role *p = boardcfg_for_step(next);
        if (!p || p->runs != BOARD_RUNS_PERSON)
            break;
        next = boardflow_next(c, next, 0);
    }
    return next;
}

const char *boardflow_fail(const struct board_card *c)
{
    const struct board_role *p = boardflow_role(c);
    if (!p || !p->fail_step[0] || !boardcfg_kind_takes(c->kind, p->fail_step))
        return NULL;
    return boardcfg_for_step(p->fail_step) ? p->fail_step : NULL;
}

int boardflow_lands(const char *kind)
{
    const struct board_role *p = boardcfg_worker();
    return p && boardcfg_kind_takes(kind, p->step);
}

const char *boardflow_person(const char *kind)
{
    const char *last = NULL;
    for (int at = 0;; at++) {
        const char *step = boardcfg_kind_step(kind, at);
        if (!step)
            return last;
        const struct board_role *p = boardcfg_for_step(step);
        if (p && p->runs == BOARD_RUNS_PERSON)
            last = p->step;
    }
}

char *boardflow_approval(const struct board_card *c)
{
    const struct board_kind *k = c ? boardcfg_kind(c->kind) : NULL;
    if (!k || !k->approval_prompt || !*k->approval_prompt)
        return NULL;

    char *say = boardcfg_expand(k->approval_prompt, c->id);
    if (!say)
        return NULL;

    for (int i = 0; i < c->log_n; i++)
        if (c->log[i].text && !strcmp(c->log[i].text, say)) {
            free(say);
            return NULL;
        }
    return say;
}

int boardflow_skippable(const struct board_card *c)
{
    const struct board_role *p = boardflow_role(c);
    return p && p->skippable;
}

int boardflow_skip(const struct board_card *c)
{
    if (!boardflow_skippable(c))
        return 0;

    char why[64];
    snprintf(why, sizeof why, "%s skipped", c->step);
    return board_move_to(c->id, boardflow_next(c, c->step, 0), "you", why);
}
