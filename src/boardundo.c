#include "boardundo.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardwork.h"
#include "gitcmd.h"
#include "text.h"

static void short_sha(const char *sha, char *out, size_t size)
{
    snprintf(out, size > 8 ? 8 : size, "%s", sha);
}

static int roll_back(const struct board_card *c, char *said, size_t size)
{
    char root[4096];
    if (!gitcmd_root(c->cwd, root, sizeof root)) {
        snprintf(said, size, "%s is not in a git repo", c->cwd);
        return 0;
    }

    char on[128];
    if (!gitcmd_line(root, "rev-parse --abbrev-ref HEAD", on, sizeof on) ||
        strcmp(on, c->merge_into)) {
        snprintf(said, size, "%s is on %s, not %s", root, on, c->merge_into);
        return 0;
    }

    char tip[48];
    if (!gitcmd_line(root, "rev-parse HEAD", tip, sizeof tip) ||
        strcmp(tip, c->merge_to)) {
        snprintf(said, size, "%s moved on since the merge", c->merge_into);
        return 0;
    }

    char dirty[256];
    if (gitcmd_line(root, "status --porcelain -uno", dirty, sizeof dirty)) {
        snprintf(said, size, "%s has uncommitted changes", root);
        return 0;
    }

    char branch[128], qbranch[300];
    boardwork_branch_of(c->id, branch, sizeof branch);
    if (!text_shell_quote(branch, qbranch, sizeof qbranch)) {
        snprintf(said, size, "bad branch name %s", branch);
        return 0;
    }

    char args[512], seen[64];
    snprintf(args, sizeof args, "rev-parse --verify %s", qbranch);
    if (!gitcmd_line(root, args, seen, sizeof seen)) {
        snprintf(args, sizeof args, "branch %s %s", qbranch, c->merge_to);
        if (!gitcmd_run(root, args)) {
            snprintf(said, size, "could not put %s back", branch);
            return 0;
        }
    }

    snprintf(args, sizeof args, "reset --hard %s", c->merge_from);
    if (!gitcmd_run(root, args)) {
        snprintf(said, size, "could not reset %s", c->merge_into);
        return 0;
    }

    char path[4200];
    boardwork_worktree_of(root, c->id, path, sizeof path);
    if (!gitcmd_worktree_add(root, path, branch)) {
        snprintf(said, size, "reset %s, but the worktree did not come back",
                 c->merge_into);
        return 0;
    }

    /* the branch is back, so merge is undone: it drops out of the history and
       the card is your turn again */
    struct board_card edited = *c;
    int k = 0;
    for (int i = 0; i < edited.done_n; i++)
        if (strcmp(edited.done[i], "merge"))
            snprintf(edited.done[k++], BOARD_ACTION_NAME, "%s", edited.done[i]);
    edited.done_n = k;
    edited.queue_n = 0;
    edited.col = BOARD_BACKLOG;
    edited.step[0] = '\0';
    snprintf(edited.worktree, sizeof edited.worktree, "%s", path);
    edited.merge_into[0] = '\0';
    edited.merge_from[0] = '\0';
    edited.merge_to[0] = '\0';
    board_update(&edited);

    char was[8];
    short_sha(c->merge_from, was, sizeof was);
    snprintf(said, size, "merge undone, %s back at %s", c->merge_into, was);
    board_note(c->id, "you", said);
    return 1;
}

int boardundo_can(const struct board_card *c)
{
    return c && board_ran(c, "merge");
}

int boardundo_run(const struct board_card *c, char *said, size_t size)
{
    if (!boardundo_can(c)) {
        snprintf(said, size, "nothing to undo");
        return 0;
    }

    if (!c->merge_to[0] || !c->merge_from[0] || !c->merge_into[0]) {
        snprintf(said, size, "no merge on record to undo");
        return 0;
    }

    return roll_back(c, said, size);
}
