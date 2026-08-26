---
tier: med
in: repo
needs: implement
closes: yes
fail marker: MERGE BLOCKED
---

The work on this card was approved. Land it. You are out of the worktree now,
in the checkout the card came from. The commands below already carry the paths
and the branch names; run them as they stand.

Check the checkout first. It must have no uncommitted change to a file the
branch touches:

    git -C {repo} status --porcelain

If something is in the way, that is not something the branch can fix: say what
it is and answer with MERGE BLOCKED.

Then rebase the branch onto the checkout's branch, from inside the worktree:

    git -C {worktree} rebase {onto}

Resolve what the rebase raises. A conflict is yours to settle: take the intent
of both sides rather than either one whole. Run the project's check afterwards
and fix what it reports, committing on the branch you rebased.

CARD.md is tracked on the branch and must not reach the checkout, in the tree
or in the history. Strip it out of the rebased commits rather than deleting it
in one on top of them, so the checkout's log carries the work and nothing else:

    FILTER_BRANCH_SQUELCH_WARNING=1 git -C {worktree} filter-branch -f \
        --prune-empty --index-filter \
        'git rm -q --cached --ignore-unmatch CARD.md' {onto}..HEAD

That rewrites each commit's index rather than replaying a patch, so it cannot
conflict. A commit that carried only CARD.md is pruned; one that carried it
alongside code keeps the code. The branch as it stood is left behind at
refs/original/refs/heads/{branch}. If nothing was rewritten, the branch never
tracked CARD.md and there is nothing to strip; carry on.

Land it with a fast-forward merge once the branch is clean and the check
passes:

    git -C {repo} merge --ff-only {branch}

If it still will not go, say what stopped it and answer with MERGE BLOCKED
rather than forcing it.

Leave the worktree and the branch where they are. The board removes both when
the card is closed.

Answer with what you merged, any conflict you settled and how, and anything the
check made you change.
