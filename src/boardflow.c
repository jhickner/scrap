#include "boardflow.h"

#include <stdio.h>

#include "boardaudit.h"

/* The column a card waits in for a role. Skipping it takes the flow from
 * `next`, or lands on `to` where what follows is not a step. */
static const struct {
    enum board_col  at;
    enum board_who  who;
    enum board_step next;
    enum board_col  to;
} STAGE[] = {
    {BOARD_NEW,     BOARD_WHO_TRIAGE, BOARD_STEPS,       BOARD_BACKLOG},
    {BOARD_UNCLEAR, BOARD_WHO_TRIAGE, BOARD_STEPS,       BOARD_BACKLOG},
    {BOARD_DOING,   BOARD_WHO_WORKER, BOARD_STEP_REVIEW, BOARD_COLS},
    {BOARD_AUDIT,   BOARD_WHO_AUDIT,  BOARD_STEP_MERGE,  BOARD_COLS},
    {BOARD_MERGING, BOARD_WHO_MERGE,  BOARD_STEPS,       BOARD_DONE},
};

enum board_col boardflow_from(const char *kind, enum board_step from,
                              int audit_worth_it)
{
    for (int i = from; i < BOARD_STEPS; i++) {
        if (!boardcfg_kind_takes(kind, (enum board_step)i))
            continue;
        switch ((enum board_step)i) {
        case BOARD_STEP_REVIEW:
            return BOARD_REVIEW;
        case BOARD_STEP_AUDIT:

            if (audit_worth_it)
                return BOARD_AUDIT;
            break;
        case BOARD_STEP_MERGE:
            return BOARD_MERGING;
        default:
            break;
        }
    }
    return BOARD_DONE;
}

enum board_who boardflow_who_at(enum board_col col)
{
    for (size_t i = 0; i < sizeof STAGE / sizeof *STAGE; i++)
        if (STAGE[i].at == col)
            return STAGE[i].who;
    return BOARD_WHO;
}

int boardflow_has_stage(enum board_who who)
{
    for (size_t i = 0; i < sizeof STAGE / sizeof *STAGE; i++)
        if (STAGE[i].who == who)
            return 1;
    return 0;
}

int boardflow_skippable(const struct board_card *c)
{
    enum board_who who = c ? boardflow_who_at(c->col) : BOARD_WHO;
    return who < BOARD_WHO && boardcfg_for(who)->skippable;
}

int boardflow_skip(const struct board_card *c)
{
    if (!boardflow_skippable(c))
        return 0;

    for (size_t i = 0; i < sizeof STAGE / sizeof *STAGE; i++) {
        if (STAGE[i].at != c->col)
            continue;
        enum board_col to =
            STAGE[i].next < BOARD_STEPS
                ? boardflow_from(c->kind, STAGE[i].next, boardaudit_wanted(c))
                : STAGE[i].to;
        char why[64];
        snprintf(why, sizeof why, "%s skipped",
                 boardcfg_who_name(STAGE[i].who));
        return board_move(c->id, to, "you", why);
    }
    return 0;
}
