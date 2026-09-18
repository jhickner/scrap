# Task operations

Task records live in `~/.config/orchestrator/projects/<name>.jsonl`, one JSON
object per line, append-only: a change is a new full record with the same id;
the newest line per id is current. Schema: task record in
references/schemas.md. `mux orch` is the only writer — it takes the file lock,
carries every field forward, and appends. Reach for an editor only to repair
something the tool cannot express, and say so when you do.

## Scope

Create records here only for user-requested product work: features, bug fixes,
implementation changes, or another durable deliverable. Ad-hoc research,
credential lookup, status investigation, and other non-product multi-step work
are not project tasks; delegate them as one-off subagents per dispatch.md.

## New project

Create and dispatch a task to make the directory under ~/working, run `git
init`, add a `.gitignore` for build output, and make an initial commit. Once it
completes, register the project in registry.json; do not perform project setup
inline. Later code tasks can then get their own worktrees. When a task in a new
project completes, have a worker commit its explicit files so the next worktree
branches from its work.

## Add

    mux orch add <project> "<description>" --class <planning|diagnosis|impl> [--checkpoint]

The project is whatever the user said: name or alias, case-insensitive. Infer
the class — design/review/ambiguity → planning, product bug hunting →
diagnosis, everything else → impl. The call answers with the new id. Confirm in
one sentence: task, project, class.

## Dedupe

Before adding, read `mux orch list` for one covering the same work. If the new
ask is a feature or tweak close to a `dispatched` task, don't ask and don't add
a task: queue it as a follow-up. For any other likely match, say so and ask
amend-or-new instead of adding a duplicate.

## Follow-ups

A follow-up goes to the task's existing worker, not through a separate task or
review cycle.

    mux orch send <task> "<instruction>"

If the worker is live it is typed at it; if not, it is queued on the task and
the reply says `queued`. Restate the completion contract when a follow-up
lands, so the worker rewrites its result file. Keep the task `dispatched`; it
reaches `review` once, after its last follow-up. If the worker's session is
gone for good, dispatch the follow-up as a new task in the same worktree.

## List / status

    mux orch list          open tasks across every project
    mux orch list all      including done and cancelled
    mux orch show <task>   one task in full, plus its live session state

Spoken form: count first, then the queued/dispatched few by short description —
never read ids aloud. "Three open in mux: the registry task is dispatched, two
are queued."

Inside mux, `/tasks` prints the same open tasks as a grouped table with task,
id, status, backend/model and age, and shows the live state of dispatched
sessions. `/tasks all` includes the closed ones.

## Edit / complete / cancel

    mux orch status <task> <status>
    mux orch note <task> "<text>"

`review` means the worker finished and the change awaits review and merge.
`done` means complete and merged, and nothing moves out of `done`. When the
user approves a `review` task, dispatch a worker to commit its worktree changes
(explicit paths) and merge the branch into the project's main branch, then set
`done` with the merge commit in notes. Manual complete (user says it's done)
sets `done` and notes "closed by user". `cancelled` is for work that will never
be done; like `done` it drops out of the open list, but unlike `done` it can be
reopened. Reassignment is a dispatch with a different backend or model.

## Dependencies

A task with unmet deps (any dep not `done`) is never dispatched; mention the
blocker when listing it.
