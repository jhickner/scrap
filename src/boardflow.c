#include "boardflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct board_action *boardflow_action(const struct board_card *c)
{
    return c && c->queue_n ? boardcfg_action(c->queue[0]) : NULL;
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

/* A gate a pipeline satisfies itself: an action already queued, or named
   earlier in the same trigger, will have run by the time this one starts. */
static int will_have_run(const struct board_card *c, const char *const *ahead,
                         int ahead_n, const char *name)
{
    if (board_ran(c, name))
        return 1;
    for (int i = 0; i < c->queue_n; i++)
        if (!strcmp(c->queue[i], name))
            return 1;
    for (int i = 0; i < ahead_n; i++)
        if (!strcmp(ahead[i], name))
            return 1;
    return 0;
}

int boardflow_trigger(const struct board_card *c, const char *const *names,
                      int n, char *why, size_t size)
{
    if (!c || n <= 0) {
        snprintf(why, size, "nothing to run");
        return 0;
    }
    if (c->queue_n + n > BOARD_QUEUE) {
        snprintf(why, size, "the queue on this card is full");
        return 0;
    }

    for (int i = 0; i < n; i++) {
        const struct board_action *p = boardcfg_action(names[i]);
        if (!p || p->on_capture) {
            snprintf(why, size, "%s is not an action to run", names[i]);
            return 0;
        }
        for (int j = 0; j < p->needs_n; j++)
            if (!will_have_run(c, names, i, p->needs[j])) {
                snprintf(why, size, "%s needs %s to have run first", names[i],
                         p->needs[j]);
                return 0;
            }
    }

    const char *queue[BOARD_QUEUE];
    int         k = 0;
    for (; k < c->queue_n; k++)
        queue[k] = c->queue[k];
    for (int i = 0; i < n; i++)
        queue[k++] = names[i];

    if (!board_queued(c->id, queue, k)) {
        snprintf(why, size, "could not queue on card %s", c->id);
        return 0;
    }
    why[0] = '\0';
    return 1;
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
