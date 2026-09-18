# Task operations

Task records live in `~/.config/orchestrator/projects/<name>.jsonl`, one JSON
object per line, append-only: a change is a new full record with the same id;
the newest line per id is current. Schema: task record in
references/schemas.md.

## Scope

Create records here only for user-requested product work: features, bug fixes,
implementation changes, or another durable deliverable. Ad-hoc research,
credential lookup, status investigation, and other non-product multi-step work
are not project tasks; delegate them as one-off subagents per dispatch.md and
do not append them to a project's JSONL.

## New project

Create and dispatch a task to make the directory under ~/working, run `git
init`, add a `.gitignore` for build output, and make an initial commit. Once
it completes, register the project in registry.json; do not perform project
setup inline. Later code tasks can then get their own worktrees. When a task
in a new project completes, have a worker commit its explicit files so the
next worktree branches from its work.

## Add

Resolve the project via registry.json. Generate id `t-` + 6 random hex.
After confirming the request is in scope above, append with class (infer:
design/review/ambiguity → planning, product bug hunting → diagnosis,
everything else → impl), status `queued`, created/updated = now
(unix seconds), deps [] unless the user names an ordering. Confirm in one
sentence: task, project, class.

## Dedupe

Before appending, check the project's open (non-done) tasks for one covering
the same work. If the new ask is a feature or tweak close to a `dispatched`
task, don't ask and don't add a task: queue it as a follow-up (below). For
any other likely match, say so and ask amend-or-new instead of adding a
duplicate.

## Follow-ups

A follow-up goes to the task's existing worker, not through a separate task
or review cycle. Append the task record with the instruction added to
`pending` and confirm in one sentence. When that task's result arrives
(monitor.md), don't move it to `review`: delete the result file, send the
pending items to the worker's slot in one message via
`{"send":"<text>","slot":N}` (dispatch.md), restating the completion
contract so it rewrites the result file, clear `pending`, keep status
`dispatched`, restart the waiter, and log a `followup` line. The task reaches
`review` once, after its last follow-up. If the worker's session is gone,
dispatch the follow-up as a new task in the same worktree.

## List / status

Read the project file, collapse to newest-per-id, filter out `done` unless
asked. Spoken form: count first, then the queued/dispatched few by short
description — never read ids aloud. "Three open in mux: the registry task is
dispatched, two are queued."

In mux, `/tasks` prints the open tasks across every orchestrator project as a
grouped table with task, status, backend/model, and age. It also shows the live
mux state of dispatched sessions. Use `/tasks all` to include done tasks.

## Edit / complete / reassign

Append a new record with the changed fields and updated timestamp. `done`
means complete and merged, never just "worker finished" (that is `review`).
When the user approves a `review` task, dispatch a worker to commit its
worktree changes (explicit paths) and merge the branch into the project's
main branch, then set `done` with the merge commit in notes. Manual complete
(user says it's done) sets status `done` and notes "closed by user".
Reassign updates backend/model and appends a `reassign` line to log.jsonl.

## Dependencies

A task with unmet deps (any dep not `done`) is never dispatched; mention the
blocker when listing it.
