# /run and /close lose the card after a restart

/run and /close lose the card after a restart

`/run implement` in a session that is plainly working a card answers "/run —
this session is not working a card". `/close` says the same, and the worker
mark is missing from that row in `/sessions`.

## What is wrong

`do_run` (cmd.c:746) asks `boardwork_card_of(s)`, which is `slot_by_session()`
(boardwork.c:53): a scan of the static `workers[WORKSPACE_MAX]` table comparing
session **pointer identity**. That table is filled only by `hold()`
(boardwork.c:189), which runs when the board starts or rejoins an action on a
card. It is process-local, and nothing rebuilds it.

So the association is lost whenever a session outlives the process, or is made
outside the board:

- After a restart. `/restart`, and the SIGURG reload that `make install` sends,
  replace the process. `restore_tabs()` (main.c:83) respawns each tab from the
  tabs file — backend, cwd, model, effort, `--session <id>` — and nothing
  repopulates `workers[]`. The card record still carries its worker's session
  id (`c->session`, written at boardwork.c:1365), so the fact survives on disk
  and is never read back.
- A session opened in a card's worktree by hand, by starting mux there or
  forking with /fh, was never held in the first place.
- A worker tab closed and taken back from `/sessions` respawns without going
  through `hold()`.

Everything keyed on that table goes with it: `/run` (cmd.c:748), `/close`
(cmd.c:731), `boardwork_step_job`, `boardwork_elapsed`, `boardwork_tab`, and
the worker mark in `/sessions`, which comes from the same call
(sessionswitch.c:84).

## The fix

Rebuild the table once at startup, after `restore_tabs()` (main.c:711). Export
`boardwork_reattach(void)`: one `board_load()`, and for each open card `hold()`
it against the tab whose session id equals `c->session`, falling back to the
tab whose cwd equals `c->worktree`. Both halves already exist —
`tree_session(c->worktree)` (boardwork.c:1265) does the cwd match for
`rebind()`, and `hold()` takes the slot.

Prefer that to a lazy fallback inside `boardwork_card_of()`. That function runs
from `livelist_on_card` on every publish and from the `/sessions` relist, so a
`board_load()` on each miss would parse a 650KB board.jsonl in the paint path.

## Traps

- Never adopt `workspace_base()`, and never a session already in a slot.
  `tree_session` skips both already.
- Two tabs in one card's worktree: match the session id first and fall back to
  cwd, or a fork gets held instead of the worker.
- A closed card, or one whose worktree is gone, must not be reattached.
- `hold()` stamps `began = now_seconds()`, which feeds `boardwork_elapsed()`.
  A reattached card would report its run as having started at the restart.
  Carry the start over or accept that it resets, but decide it rather than
  leaving it to fall out.

worktree /Users/jhickner/working/mux/.claude/worktrees/ufy3
