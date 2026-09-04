#include "boardflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"

_Static_assert(BOARD_PIPELINE_LONG <= BOARD_QUEUE,
               "a pipeline has to fit in a card's queue");

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

/* An action may declare that it finishes the card. It only does so as the last
   thing queued: closing drops the queue, and an action behind it was asked for
   after this one, so it has not been answered yet. */
int boardflow_closes(const struct board_card *c, const struct board_action *p)
{
    return c && p && p->closes && c->queue_n == 1 && !strcmp(c->queue[0], p->name);
}

int boardflow_waits_on_you(const struct board_card *c)
{
    return board_stands(c) == BOARD_REVIEW;
}

/* A name is an action, or a pipeline standing for a run of them. Flattening
   first means a pipeline is gated action by action, like a typed-out list. */
static int flatten(const char *const *names, int n, const char **out, int max)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const struct board_pipeline *p = boardcfg_pipeline(names[i]);
        if (!p) {
            if (k == max)
                return -1;
            out[k++] = names[i];
            continue;
        }
        for (int j = 0; j < p->actions_n; j++) {
            if (k == max)
                return -1;
            out[k++] = p->actions[j];
        }
    }
    return k;
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

static int queued(const struct board_card *c, const char *name)
{
    for (int i = 0; i < c->queue_n; i++)
        if (!strcmp(c->queue[i], name))
            return 1;
    return 0;
}

static int gates_met(const struct board_card *c, const char **flat, int n,
                     char *why, size_t size)
{
    for (int i = 0; i < n; i++) {
        const struct board_action *p = boardcfg_action(flat[i]);
        if (!p || p->on_capture) {
            snprintf(why, size, "%s is not an action to run", flat[i]);
            return 0;
        }
        for (int j = 0; j < p->needs_n; j++)
            if (!will_have_run(c, flat, i, p->needs[j])) {
                snprintf(why, size, "%s needs %s to have run first", flat[i],
                         p->needs[j]);
                return 0;
            }
    }
    return 1;
}

/* Whether triggering this name right now would be taken, which is what makes a
   pipeline offerable: every action in it has to clear its gate in turn. */
static int takeable(const struct board_card *c, const char *name)
{
    const char *flat[BOARD_QUEUE];
    char        why[128];

    int n = flatten(&name, 1, flat, BOARD_QUEUE);
    for (int i = 0; i < n; i++)
        if (queued(c, flat[i]))
            return 0;
    return n > 0 && gates_met(c, flat, n, why, sizeof why);
}

/* How far along the card an action sits: the longest run of gates behind it.
   The bound stops a config whose gates name each other in a circle. */
static int depth(const char *name, int left)
{
    const struct board_action *p = boardcfg_action(name);
    if (!p || left <= 0)
        return 0;

    int deep = 0;
    for (int i = 0; i < p->needs_n; i++) {
        int d = 1 + depth(p->needs[i], left - 1);
        if (d > deep)
            deep = d;
    }
    return deep;
}

int boardflow_offered(const struct board_card *c, const char **out, int max)
{
    const char *all[BOARD_ACTIONS_MAX];
    int         k = 0;

    int n = boardcfg_actions(all, BOARD_ACTIONS_MAX);
    for (int i = 0; i < n && k < max; i++)
        if (boardflow_gated(c, all[i]) && !queued(c, all[i])) {
            int at = k++;
            int deep = depth(all[i], BOARD_ACTIONS_MAX);
            for (; at > 0 && depth(out[at - 1], BOARD_ACTIONS_MAX) < deep; at--)
                out[at] = out[at - 1];
            out[at] = all[i];
        }

    n = boardcfg_pipelines(all, BOARD_ACTIONS_MAX);
    for (int i = 0; i < n && k < max; i++)
        if (takeable(c, all[i]))
            out[k++] = all[i];
    return k;
}

int boardflow_trigger(const struct board_card *c, const char *const *names,
                      int n, char *why, size_t size)
{
    if (!c || n <= 0) {
        snprintf(why, size, "nothing to run");
        return 0;
    }

    const char *flat[BOARD_QUEUE];
    int         k = flatten(names, n, flat, BOARD_QUEUE);
    if (k < 0 || c->queue_n + k > BOARD_QUEUE) {
        snprintf(why, size, "the queue on this card is full");
        return 0;
    }
    for (int i = 0; i < k; i++)
        if (queued(c, flat[i])) {
            snprintf(why, size, "%s is already queued", flat[i]);
            return 0;
        }
    if (!gates_met(c, flat, k, why, size))
        return 0;

    const char *queue[BOARD_QUEUE];
    int         at = 0;
    for (; at < c->queue_n; at++)
        queue[at] = c->queue[at];
    for (int i = 0; i < k; i++)
        queue[at++] = flat[i];

    if (!board_queued(c->id, queue, at)) {
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

int boardflow_exclusive(const struct board_action *p)
{
    return p && p->where == BOARD_IN_REPO;
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
