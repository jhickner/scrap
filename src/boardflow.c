#include "boardflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *boardflow_running(const struct board_card *c)
{
    return c && c->queue_n ? c->queue[0] : NULL;
}

const struct board_action *boardflow_action(const struct board_card *c)
{
    const char *at = boardflow_running(c);
    return at ? boardcfg_action(at) : NULL;
}

int boardflow_gated(const struct board_card *c, const char *name)
{
    const struct board_action *p = boardcfg_action(name);
    if (!c || !p || p->on_capture)
        return 0;
    for (int i = 0; i < p->needs_n; i++)
        if (!board_ran(c, p->needs[i]))
            return 0;
    return 1;
}

int boardflow_offered(const struct board_card *c, const char **out, int max)
{
    const char *all[BOARD_ACTIONS_MAX];
    int         n = boardcfg_actions(all, BOARD_ACTIONS_MAX);

    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (boardflow_gated(c, all[i]))
            out[k++] = all[i];
    return k;
}

int boardflow_waits_on_you(const struct board_card *c)
{
    return board_stands(c) == BOARD_REVIEW;
}

static int wants_tree(const char *name)
{
    const struct board_action *p = boardcfg_action(name);
    return p && p->where == BOARD_IN_WORKTREE;
}

int boardflow_worktree(const struct board_card *c)
{
    if (!c)
        return 0;
    for (int i = 0; i < c->queue_n; i++)
        if (wants_tree(c->queue[i]))
            return 1;
    for (int i = 0; i < c->done_n; i++)
        if (wants_tree(c->done[i]))
            return 1;
    return 0;
}

/* merge and anything gated on it act on the checkout the card came from, not
   on the branch, so the session moves out of the worktree to take them. */
const char *boardflow_cwd(const struct board_card *c,
                          const struct board_action *p)
{
    if (!c)
        return NULL;
    if (p && p->where == BOARD_IN_REPO)
        return c->cwd;
    return c->worktree[0] ? c->worktree : c->cwd;
}
