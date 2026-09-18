# Dispatch

## Model selection

Before every selection, run `~/.config/orchestrator/quota.sh` (real readings
from mux's agenttabs cache, codex session logs as fallback) and re-anchor each
backend it reports in usage.json: `remaining_pct`, `resets_at`, `read_at`,
`source: "rate_limit"`, clearing drift. A null backend (grok has no quota
source) keeps its last value; never treat an unread placeholder as full quota.

Read routing.json for the task's class candidates and usage.json for quota.
For a one-off, infer the closest class for routing only; this does not create a
project task. Projected remaining per backend = `remaining_pct` − Σ inflight
`est` − the candidate's `est` (task_cost for that class, else routing
seed_cost). Dispatch to the highest projected value; on dispatch, append the
prediction to that backend's `inflight` and bump `dispatches`.

## Spawning

Workers are sibling sessions in this mux instance, spawned via the dispatch
request file (src/dispatch.c in the mux repo):

    printf '{"backend":"claude","model":"opus[1m]","cwd":"<project cwd>","prompt":"<worker prompt>","title":"<task hash> <task short description>"}' \
      > ~/.config/mux/dispatch/$MUX_PID-<request-key>.req

For a project task, the request key is its task id and the title begins with
the task id without its `t-` prefix (e.g. `6beac7 deploy rmchores`). For a
one-off, use a transient `o-` plus 6 random hex key and a descriptive title.

Poll for `$MUX_PID-<request-key>.res` (a few seconds); it holds the slot. The
session id appears in the live registry (`~/.config/mux/live`) shortly after.
Record it in the project task record only for project work. Delete the .res
after reading.

`{"send":"<text>","slot":N}` types a message into a live worker's slot
(reply `{"ok":true}` or `{"error": ...}`); used for follow-ups.

The same directory takes `{"close":"<session id>"}` (or `{"close":<slot>}`)
to close a tab; the reply is `{"ok":true,"slot":N,"session":"<id>"}` or
`{"error": "no such slot or session" | "slot is in view" | "slot is busy"}`.

For code tasks, first create a worktree `.claude/worktrees/<task-id>` (branch
`worktree-<task-id>`) in the project repo and use it as cwd. Tasks whose deps
are all done dispatch in parallel, each in its own worktree; never serialize
independent tasks for lack of a commit — make the commit instead.

## One-off non-product work

Ad-hoc research, credential lookup, status investigation, and other
non-product multi-step requests go to a one-off subagent. Do not create or
append a project task record, create a worktree, or put the request through
queued/review/done lifecycle. Give it only the access and scope needed to
answer the request. Its prompt must write a transient result to
`~/.config/orchestrator/results/<one-off-key>.json` using the one-off result
schema in schemas.md. Wait for the result, report it to the user, account for
actual quota usage, then delete the result file.

## Worker prompt

For a project task, always include: the task id and description; the project
cwd; an instruction to read any spec files; scope limits (touch nothing
outside scope, no commits unless the task says so); "you are a worker — do not
invoke the orchestrate skill or dispatch further workers"; and the completion
contract — "as your final act write
~/.config/orchestrator/results/<task-id>.json per the result schema in this
skill's references/schemas.md, with a one-paragraph summary sized for being
read aloud."

For a one-off, include the request and relevant cwd, prohibit product changes,
commits, and further dispatch, identify it as a one-off subagent rather than a
project task, and include the transient completion contract above.

## Watcher

After dispatching, start a background waiter on the result file(s) (a Bash
run_in_background loop polling `~/.config/orchestrator/results/`, or fswatch
if available) so completion re-invokes you without user prompting. Reconcile
project tasks per references/monitor.md when it fires; consume one-off results
directly as described above. Long dispatches also get a fallback heartbeat:
if nothing has fired in ~20 minutes, reconcile anyway and probe anything
overdue.

## After spawning

For project work, append the task record update (status `dispatched`, backend,
model, session, worktree) and a `dispatch` line to log.jsonl. For a one-off,
do neither. Tell the user in one sentence who got the request.
