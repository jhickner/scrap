# Monitoring

## Reconcile (run on any status question, and each loop tick)

1. Read new files in `~/.config/orchestrator/results/` — a task with
   non-empty `pending` gets its follow-ups sent instead (tasks.md,
   Follow-ups); for the rest, append the
   task update (result `done` → task `review`, `failed` → `failed`, cost;
   a task becomes `done` only once merged — see tasks.md), remove the backend's matching
   `inflight` entry in usage.json and fold the observed spend into
   `task_cost.est` (EMA, bump `n`), append a `result` log line.
2. Scan `~/.config/mux/live` for recorded session ids: present = still
   running; gone with no result file = suspect, note it. Finished worker
   tabs stay open; close one only when the user asks.
3. Dispatch any queued task whose deps just completed, unless the project is
   paused on a checkpoint.

## Restart reconcile

The reconcile above is also the crash/restart recovery: state files are the
only truth, so a fresh session that runs it resumes cleanly. Additionally:
any usage.json `inflight` entry whose task has a result file gets folded in
normally; one with no result and no live session is an orphan — mark the
task `failed` with note "lost across restart" and tell the user. Re-start a
background waiter for every task still `dispatched`.

## Daily summary

On the first interaction of a calendar day (compare the newest log.jsonl
timestamp), open with one sentence from the last day's log — completions,
failures, quota spent — before handling the request. Skip it if the log is
empty.

## Digests

Speak only changes: finished, failed, newly stuck. One sentence per item,
task description not id. Nothing changed → say nothing (loop tick) or "all
quiet, N still running" (asked).

## Probes

For a worker running long with no result: fork its session read-only —
`mux --session <id> --fork "brief status: what are you doing and how far
along?"` — and relay a one-line summary. Log a `probe` line. Never probe more
than once per task per ~10 minutes.

## Stuck / failed

A worker gone from the live dir without a result file, or a probe describing
a loop: mark `failed` with a note, tell the user, and ask whether to
redispatch (same or different backend) — redispatch costs quota, so it is
the user's call.

## Checkpoints

When a checkpoint task completes: pause dispatching for that project, say
what's ready to test, and wait. The reply's final sentence must be a concise
test pointer: where the change lives and what to exercise. Feedback becomes
new or amended task records; then resume.
