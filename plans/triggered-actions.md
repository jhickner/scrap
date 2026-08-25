# Triggered actions

Replace the per-kind step pipeline with a set of actions you trigger on a card,
gated on what has already run. Review stops being a configured step and becomes
the resting state a card lands in when its queue empties.

## The model

A card is an idea. You trigger an action on it, or a named pipeline of several.
Each action declares what it needs to have run first and which directory it runs
in. The card's one session takes every action, as it does today.

    plan        needs: —           in: worktree
    implement   needs: —           in: worktree
    audit       needs: implement   in: worktree
    test        needs: implement   in: worktree
    merge       needs: implement   in: repo
    deploy      needs: merge       in: repo

Triggering `plan` alone runs it and stops. Triggering the `quick` pipeline runs
`implement, merge` and stops. Either way the card ends in review, which means
nothing is queued and it is your turn.

A single action is a one-element pipeline, so there is one mechanism, not two.

## Card state

`col` and `step` go. In their place:

    char queue[BOARD_QUEUE][BOARD_ACTION_NAME];  int queue_n;   /* still to run */
    char done[BOARD_DONE_MAX][BOARD_ACTION_NAME]; int done_n;   /* ran and passed */

The column is derived, not stored:

    nothing run, nothing queued, never stopped  -> open
    something queued or running                 -> working
    queue empty over a history, or stopped      -> review
    closed by hand                              -> done

There is no `unclear`: naming never asks you anything, so nothing lands there.
Nor is there `new` or `backlog`: nothing moved a card between them once triage
stopped classifying, so the whole column collapses to a `closed` bit on the
card. `board_where()` goes with it; callers that print where a card is ask
`board_stands()` and name the stand.

A gate is satisfied when every name in `needs:` is in `done`. `done` is appended
only on success, which is the point: `deploy needs: merge` has to know the merge
worked, and a note under the merge job exists whether it worked or not.

## Config surface

`board/kinds/` is deleted. With no classifier there is nothing for `means` to
feed, `steps` is replaced by pipelines, and `next kind`, `approval prompt` and
`worktree` all lose their reason to exist. `priority` moves onto the card, where
it already lives as a field.

`board/roles/` becomes `board/actions/`, and an action file shrinks to:

    ---
    tier: high
    in: worktree          # or repo
    needs: implement      # comma separated, empty for none
    lock: repo            # unchanged, merge still needs it
    ---
    <the prompt>

Gone from the role: `runs` (every action is a worker action now), `step` (the
action's name is the step), `skippable`, `fail marker`, `fail step`,
`fail prompt`, `pass label`, `fail label`, `over files`, `over lines`.

`board/pipelines/` is new and just as thin:

    ---
    actions: implement, merge
    ---

Both directories are compiled in by `tools/gen-defaults.sh` exactly as roles and
kinds are today. No new build machinery.

## Code

**board.h / board.c.** Drop `enum board_col` and the `step` field; add `queue`
and `done`. `board_move`/`board_move_to`/`board_move_back` are replaced by
`board_queue(id, actions, n)`, `board_took(id, action)` (append to done, pop the
queue) and `board_close(id)`. `board_cmp_col` sorts on the derived column.
`where_from_name` keeps its alias table for reading old cards — see Migration.

**boardflow.c.** `boardflow_next`, `boardflow_start`, `boardflow_after_turn`,
`boardflow_fail`, `boardflow_skip`, `boardflow_skippable`, `boardflow_approval`,
`boardflow_person`, `boardflow_runs` all go. What replaces them:

    int   boardflow_gated(const struct board_card *c, const char *action);
    int   boardflow_offered(const struct board_card *c, const char **out, int max);
    const char *boardflow_running(const struct board_card *c);   /* queue head */
    int   boardflow_waits_on_you(const struct board_card *c);    /* derived review */

`boardflow_worktree(c)` becomes "some action in the queue or in done wanted a
worktree", so the tree is made lazily by the first action that asks and is not
made at all for a card that only ever gets planned.

**boardcfg.** `struct board_kind` and everything reading it go:
`boardcfg_kind`, `boardcfg_kind_takes`, `boardcfg_kind_step`,
`boardcfg_kind_step_at`, `boardcfg_kind_steps`, `boardcfg_kinds_block`,
`boardcfg_priority`, `boardcfg_steps`, `order_steps`. `struct board_role`
becomes `struct board_action` with the fields above. `enum board_runs` and
`boardcfg_runs_name`/`_from_name` go entirely. Add `boardcfg_action(name)`,
`boardcfg_actions(out, max)`, `boardcfg_pipeline(name)`.

**boardwork.c.** `step_start` becomes "is the queue head runnable here" and
`step_send` gains a cwd move: an action with `in: repo` calls `session_set_cwd`
to the main checkout before the turn, one with `in: worktree` moves back, making
the worktree lazily if it is not there yet. `retarget` already does the cwd half
of this. `boardwork_finished` appends to `done` on success and pops the queue;
on failure it clears the queue and leaves the card in review with a note, rather
than sending it back a step. `boardwork_approve`, `boardwork_send_back` and
`boardwork_reject` collapse into `boardwork_stop` (clear the queue) — there is
nothing to approve when review is a resting state, only more to trigger.
`/approve` becomes `/close` and the board's `a` key closes rather than approves.

**boardstep.c.** Unchanged in shape; `boardstep_finished` loses the fail-marker
branch, which moves to `boardwork_finished`.

**boardtriage.c becomes boardname.c**, since triage is not what it does any
more. It keeps its shape — a headless `child_start` on a low tier, answering
with JSON — and loses everything but the title: no `kind`, no `spec`, no `cwd`,
no `confidence`, no `question`. It fires on capture, before any session exists,
which is why it stays headless rather than becoming an ordinary action. It only
bothers on a card long enough that its own first line reads badly.

Naming cannot fail in a way you need to answer, so it never blocks: the card is
open the moment it is captured, carrying `board_title_of`'s first line, and the
title improves a second later if the call comes back. That deletes the
`unclear` column outright and with it `boardtriage_attempts`, the re-triage
path in `save_card`, and `boardcfg_projects_block` — the repo comes from the
capture directory now.

It lives at `board/actions/name.md` with `on: capture`, and `boardflow_offered`
skips anything carrying that key so it never appears in the trigger menu. One
extra field rather than a second directory and a second loader.

Naming is the only thing left on the `low` tier once it stops classifying, so
`low` on claude moves from `sonnet` to `haiku` in `backend_defaults`. The other
backends already point low at their small model. This changes the seed only —
a config written by an older build keeps whatever is in
`board/backends/claude.md` until that line is edited.

**boardfile.c** is untouched and gets more useful: CARD.md is now the document a
card carries across every action, and the copy in `board-cards/` is what makes
it survive the worktree going away at merge.

**boardundo.c** goes. Nothing sets `merge_into`/`merge_from`/`merge_to` once
`merge` is not a shipped action, so `board_ran(c, "merge")` can never be true
and the whole module is unreachable. The three card fields go with it.

## Views

The list view builds lanes as `new, unclear, backlog, <every step name>, done`
(`build_board`, `lane_cards`). That becomes four fixed lanes — open, working,
review, done — with the working lane showing each card's running action as its
status rather than splitting into a lane per action. Fewer lanes, and they stop
changing shape when an action file is added.

The grid view lays out the same lanes and needs the same change. `boardtile.c`
already prints the running job from `boardwork_step_job`, so tiles mostly hold.
`boardgrid.c` is already generic over lanes — it takes `lane_of`, `lane_name`
and a count — so it needs nothing, and its `col` is a terminal column, not a
board one. What is left is vocabulary: `vrow.col`, `anchor.col` and
`backlog_shown` name a thing the board no longer has.

`boardcard.c`'s form loses the `kind` chooser and the `column` chooser, and
gains a trigger: a list of the actions whose gates are met, plus the pipelines.
That is the main new UI, and it is also reachable from a key on the board.

`/moveto <step>` becomes `/run [<action>, ...]`, the same trigger from inside
the worker session that holds the card. With no argument it names what the
card's gates currently allow. Both it and the form call `boardflow_trigger`,
which flattens any pipeline name to the actions it stands for and then gates
each one on the history plus everything queued ahead of it, so `/run merge`
alone is refused where `/run implement, merge` and `/run ship` are taken.
`boardflow_offered` lists the actions whose gates are met and then the
pipelines that would be taken whole, so the form's chooser carries both.

## Migration

Existing cards in `board.jsonl` carry `col` and `step` strings and will keep
loading, since `where_from_name` already maps unknown names through an alias
table. Map on read:

    new, unclear, backlog        -> open, queue empty, done empty
    <a step name>                -> open, with every step before it in done
    done                         -> done

That last one is a guess about history rather than a record of it, which is
honest enough for cards mid-flight at the moment of the change. Cards already
done are unaffected. Nothing needs a migration pass over the file; the mapping
happens in `load`, and the new shape is written back on the next update.

## Order of work

1. `struct board_action` and `board/actions/`, still driving the old flow, so
   the config change lands on its own and `make check` stays green. **Done.**
2. Card `queue` and `done`, the derived column, and the read migration. **Done.**
3. `boardflow` rewritten to gates; `boardwork` to queue head, with the cwd move.
   **Done.**
4. Pipelines and the trigger UI. **Done.**
5. Delete kinds, triage, and the approve/send-back path. **Done.**
6. Views. **Done.** Mostly fell out of step 3, which already laned both views
   by `board_stands`. The grid was still anchoring on `c->col` while the list
   anchored on the stand, so the cursor lost its place when the views were
   switched; both read the stand now, and `col` is gone from their vocabulary.

Steps 1–3 are the change; 4–6 are consequences. Each stops at a green build.

## Traps

**Still open: the cwd move is untested.** `step_send` calls `session_set_cwd`
when the queue head asks to run somewhere the session is not, which is the
mechanism `in: repo` rests on. Nothing shipped uses `in: repo`, so nothing
exercises it, and `boardtest` reasons about `boardflow_cwd` rather than moving a
live session. The first action that lands with `in: repo` should come with a
test that actually moves one.

**A failed action must not silently satisfy a gate.** `done` is appended in
exactly one place, on a turn that neither errored nor tripped a marker. Anywhere
else and `deploy` runs on an unmerged branch.

**Still open: nothing locks the main checkout.** `lock: repo` was parsed and
never read, so it went with the other dead fields. Nothing shipped runs in the
checkout today, which is why that was safe. Two cards running an `in: repo`
action at once is the failure the lock existed for, so it has to come back
before such an action ships — read this time, not just parsed.

**Still open: what closes a card.** Nothing moves a card to done by itself.
Closing by hand from review is the assumption, with `a` on the board, `A` for
every card in review at once, and `/close` from the session holding it. If that
turns out to be a chore, the alternative is an action declaring itself
terminal.

**Settled: a pipeline does not survive a failure mid-way.** The queue is
dropped and the card stands in review, waiting to be told what to run next.
Nothing resumes by itself, so no third state between working and review is
needed. The card carries a `stopped` bit for this: an empty queue over an empty
history would otherwise read as a card nothing had ever been run on, which is
the one case a failed first action would land in. `stopped` clears when
something is queued again or an action passes.

**Settled: what shape a name takes.** At most five words, lower case, no
trailing full stop, naming the thing rather than describing the work on it —
`tab strip wraps at 80` and not `investigate the tab wrapping problem`. The
kebab-case slug was the other candidate; it reads well in a dense list but the
branch already comes from the card id, so it bought nothing.
