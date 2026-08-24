#include "boardflow.h"

#include <stdio.h>
#include <string.h>

#include "boardaudit.h"

/* The step a card in each column is waiting on. */
static const struct {
    enum board_col  at;
    enum board_step step;
} COLUMN[] = {
    {BOARD_DOING,   BOARD_STEP_WORKTREE},
    {BOARD_REVIEW,  BOARD_STEP_REVIEW},
    {BOARD_AUDIT,   BOARD_STEP_AUDIT},
    {BOARD_MERGING, BOARD_STEP_MERGE},
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

const char *boardflow_approval(const struct board_card *c)
{
    const struct board_kind *k = c ? boardcfg_kind(c->kind) : NULL;
    if (!k || !k->approval_prompt || !*k->approval_prompt)
        return NULL;

    for (int i = 0; i < c->log_n; i++)
        if (c->log[i].text && !strcmp(c->log[i].text, k->approval_prompt))
            return NULL;
    return k->approval_prompt;
}

enum board_step boardflow_after_turn(const struct board_card *c)
{
    const struct board_kind *k = c ? boardcfg_kind(c->kind) : NULL;
    if (k && k->approval_prompt && *k->approval_prompt && !boardflow_approval(c))
        return BOARD_STEP_AUDIT;
    return BOARD_STEP_REVIEW;
}

enum board_step boardflow_step_at(enum board_col col)
{
    for (size_t i = 0; i < sizeof COLUMN / sizeof *COLUMN; i++)
        if (COLUMN[i].at == col)
            return COLUMN[i].step;
    return BOARD_STEPS;
}

int boardflow_skippable(const struct board_card *c)
{
    if (!c)
        return 0;
    const struct board_role *p = boardcfg_for_step(boardflow_step_at(c->col));
    return p && p->skippable;
}

int boardflow_skip(const struct board_card *c)
{
    if (!boardflow_skippable(c))
        return 0;

    enum board_step step = boardflow_step_at(c->col);
    char            why[64];
    snprintf(why, sizeof why, "%s skipped", boardcfg_step_name(step));
    return board_move(c->id,
                      boardflow_from(c->kind, (enum board_step)(step + 1),
                                     boardaudit_wanted(c)),
                      "you", why);
}
