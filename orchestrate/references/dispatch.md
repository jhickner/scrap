# Dispatch

## Model selection

Before every selection, run `~/.config/orchestrator/quota.sh` (real readings
from mux's agenttabs cache, codex session logs as fallback) and re-anchor each
backend it reports in usage.json: `remaining_pct`, `resets_at`, `read_at`,
`source: "rate_limit"`, clearing drift. A null backend (grok has no quota
source) keeps its last value; never treat an unread placeholder as full quota.

Read routing.json for the task's class candidates and usage.json for quota.
Projected remaining per backend = `remaining_pct` − Σ inflight `est` − the
candidate's `est` (task_cost for that class, else routing seed_cost).
Dispatch to the highest projected value; on dispatch, append the prediction
to that backend's `inflight` and bump `dispatches`.

## Spawning

Workers are sibling sessions in this mux instance, spawned via the dispatch
request file (src/dispatch.c in the mux repo):

    printf '{"backend":"claude","model":"opus[1m]","cwd":"<project cwd>","prompt":"<worker prompt>","title":"<task hash> <task short description>"}' \
      > ~/.config/mux/dispatch/$MUX_PID-<task-id>.req

The title is the worker's tab name; `<task hash>` is the task id without its
`t-` prefix (e.g. `6beac7 deploy rmchores`).

Poll for `$MUX_PID-<task-id>.res` (a few seconds); it holds the slot. The
session id appears in the live registry (`~/.config/mux/live`) shortly after
— record it in the task record. Delete the .res after reading.

`{"send":"<text>","slot":N}` types a message into a live worker's slot
(reply `{"ok":true}` or `{"error": ...}`); used for follow-ups.

The same directory takes `{"close":"<session id>"}` (or `{"close":<slot>}`)
to close a tab; the reply is `{"ok":true,"slot":N,"session":"<id>"}` or
`{"error": "no such slot or session" | "slot is in view" | "slot is busy"}`.

For code tasks, first create a worktree `.claude/worktrees/<task-id>` (branch
`worktree-<task-id>`) in the project repo and use it as cwd. Tasks whose deps
are all done dispatch in parallel, each in its own worktree; never serialize
independent tasks for lack of a commit — make the commit instead.

## Worker prompt

Always include: the task id and description; the project cwd; an instruction
to read any spec files; scope limits (touch nothing outside scope, no
commits unless the task says so); "you are a worker — do not invoke the
orchestrate skill or dispatch further workers"; and the completion contract — "as your
final act write ~/.config/orchestrator/results/<task-id>.json per the result
schema in this skill's references/schemas.md, with a one-paragraph
summary sized for being read aloud."

## Watcher

After dispatching, start a background waiter on the result file(s) (a Bash
run_in_background loop polling `~/.config/orchestrator/results/`, or fswatch
if available) so completion re-invokes you without user prompting. Reconcile
per references/monitor.md when it fires. Long dispatches also get a fallback
heartbeat: if nothing has fired in ~20 minutes, reconcile anyway and probe
anything overdue.

## After spawning

Append the task record update (status `dispatched`, backend, model, session,
worktree) and a `dispatch` line to log.jsonl. Tell the user in one sentence
who got the task.
