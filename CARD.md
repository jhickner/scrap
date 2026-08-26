# CARD.md lives in git, not in a board-kept copy

CARD.md lives in git, not in a board-kept copy

The board keeps two copies of CARD.md: the one in the worktree and a verbatim
copy at ~/.config/mux/board-cards/<id>.md, reconciled by boardfile_keep() at the
end of every worker turn. Replace that with git. The board writes CARD.md once
when the worktree is made, it is tracked on the card's branch, whoever works the
card commits their changes to it, and it is removed on the branch before the
merge so master never carries it.

That makes the branch the single record, gives the plan a history instead of
last-write-wins, and carries it to any clone rather than one machine's config
directory.

## What changes

- src/boardfile.c and src/boardfile.h: delete. boardfile_keep, boardfile_put,
  boardfile_kept and boardfile_drop all go, with their call sites in
  boardwork.c (:879, :1131, :1426) and board.c (:878).

- src/boardwork.c: ignore_card_file() (:265) goes. It appends CARD.md to
  <git-common-dir>/info/exclude, which is the shared git dir, so the line is
  repo-wide and permanent and it blocks the first `git add CARD.md` on every
  repo that already has one. Existing repos need that line removed.

- src/boardwork.c: write_card_file() (:333) loses its boardfile_put branch and
  always writes card_write(). It already runs once, at worktree creation (:623).

- src/boardwork.c: drop_worktree() (:1131) must not discard an uncommitted
  CARD.md. tidy_step runs `git worktree remove --force` and then
  `git branch -D` (:227), so the boardfile_keep call being deleted is the only
  thing standing between a dirty plan and force plus -D. Refuse to drop a dirty
  worktree, or commit CARD.md first, and say which.

## The prompts

The action prompts are compiled in: read_actions reads only board_defaults
(boardcfg.c:243) and boardcfg.c:435 deletes stale copies from the config
directory, so editing board/actions/*.md means regenerating
src/boarddefaults.c and rebuilding.

- board/actions/plan.md: the plan goes into a `## Plan` section of CARD.md and
  is committed, not into plans/<slug>.md. Those two writes are what the action
  permits.

- board/actions/implement.md: "The plan below was approved" is false. The first
  turn inlines the card from the store, not the file. Point it at CARD.md's
  `## Plan` section and say that section supersedes the card body.

- board/actions/merge.md: strip CARD.md on the branch before landing it. The
  merge is `git merge --ff-only {branch}`, so master gets exactly the branch's
  commits and a CARD.md still tracked at the tip lands at the root of master.
  Stripping on the branch also stops two cards conflicting on the same path.
  The plan stays recoverable from master's history.

- src/boardwork.c:464: "Do not open it to start: it holds nothing you have not
  been given" is false once CARD.md carries the plan. Say to read it.

## Traps

- A card closed without merging gets `git branch -D` and loses its plan, where
  the kept copy survives that today. Acceptable, but say what is being deleted.

- A card with no worktree never had a CARD.md and still does not.

- A worktree re-made for a card whose branch still exists picks CARD.md back up
  from the branch (worktree_take, gitcmd.c:68), which is what the kept copy
  used to do.

worktree /Users/jhickner/working/mux/.claude/worktrees/gss3
