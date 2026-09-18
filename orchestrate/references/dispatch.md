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

## Dispatching a project task

    mux orch dispatch <task-id> "<worker prompt>" \
      --backend claude --model "opus[1m]" --cwd <worktree> --title "<hash> <short desc>"

That one call does the mechanical part: it picks the instance, writes the
worker request, waits for the session id, records backend, model, session and
instance on the task, moves it to `dispatched`, logs the dispatch, and arms the
completion notification. It answers with the task, the session id and the pid,
or with an error and no state change.

Instance choice is not yours either: the task's origin instance is preferred,
then the most recently active instance with a free tab. If no instance has
room, the dispatch is refused and the task stays queued — say so and move on
rather than forcing it somewhere.

A task that is already `dispatched` with a live worker is refused. That is
deliberate: two workers on one task have collided in the same worktree before.
To redirect live work, send a follow-up instead.

For code tasks, create the worktree first — `.claude/worktrees/<task-id>`,
branch `worktree-<task-id>` — and pass it as `--cwd`. Tasks whose deps are all
done dispatch in parallel, each in its own worktree; never serialize
independent tasks for lack of a commit, make the commit instead.

## Follow-ups

`mux orch send <task> "<text>"` types at the task's live worker. If the worker
is gone, the text is queued on the task instead and the reply says so.

## One-off non-product work

Ad-hoc research, credential lookup, status investigation, and other non-product
multi-step requests go to a one-off subagent, spawned directly through mux's
own dispatch directory rather than through `mux orch`:

    printf '{"backend":"claude","model":"opus[1m]","cwd":"<cwd>","prompt":"<prompt>","title":"<short desc>"}' \
      > ~/.config/mux/dispatch/$MUX_PID-<o-key>.req

Poll for `$MUX_PID-<o-key>.res`; mux holds that reply until the backend reports
its session id, up to 30 seconds, then answers `{"session":"<id>","addr":"<path>"}`.
`{"error":"session id unavailable"}` means the worker is unaddressable — treat
the dispatch as failed. Delete the .res after reading. Do not create a project
task record, a worktree, or a lifecycle for a one-off. Its prompt must write a
transient result to `~/.config/orchestrator/results/<o-key>.json` using the
one-off result schema in schemas.md; read it, report it, delete it.

A session id is the only address the dispatch API accepts; there is no
positional addressing. `{"send":...}` and `{"close":...}` take one too.

## Worker prompt

For a project task, always include: the task id and description; the project
cwd; an instruction to read any spec files; scope limits (touch nothing outside
scope, no commits unless the task says so); "you are a worker — do not invoke
the orchestrate skill or dispatch further workers"; and the completion contract
— "as your final act write `~/.config/orchestrator/results/<task-id>.json` per
the result schema in this skill's references/schemas.md, with a one-paragraph
summary sized for being read aloud."

A worker can name its own session: `$MUX_SESSION_FILE` holds the path of a file
containing its session id, once the backend has reported one. Tell a worker
about it only when the work needs it.

For a one-off, include the request and relevant cwd, prohibit product changes,
commits, and further dispatch, identify it as a one-off subagent rather than a
project task, and include the transient completion contract above.

## No watcher

Do not start a background waiter, an fswatch, or a polling loop. The instance
hosting the worker records its ending — finished turn or death — and the
orchestrator subsystem delivers that to you as a line beginning
"orchestrator:", then reconciles. A completion that is somehow missed is picked
up by the periodic reconcile, which runs about every twenty minutes
(`orchestrator_reconcile_minutes` in mux settings) and on every start.

## After dispatching

Tell the user in one sentence who got the request. The task record and the log
line are already written.
