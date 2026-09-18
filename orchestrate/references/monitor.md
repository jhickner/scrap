# Monitoring

## Who does what

The subsystem folds in results, notices dead workers and keeps the task records
straight. You read the outcome and say something useful about it. When a line
arriving in your session begins "orchestrator:", that is the subsystem: it has
already moved the record, so do not move it again — read the state, tell the
user, and decide what happens next.

## Reconcile

`mux orch reconcile` runs it on demand; the subsystem also runs it on start and
on a timer. One pass does:

1. Every open task with a result file: `done` → `review`, anything else →
   `failed`, the result file consumed, a `result` line logged.
2. Every `dispatched` task whose session is gone from `~/.config/mux/live` with
   no result: `failed`, noted "worker ended without writing a result".
3. You are told about each one.

After a reconcile, fold the quota side in yourself: remove the backend's
matching `inflight` entry in usage.json and fold the observed spend into
`task_cost.est` (EMA, bump `n`).

A task with queued follow-ups (`pending` non-empty) that reports a result is
not finished: send the follow-ups with `mux orch send`, restating the
completion contract so the worker rewrites its result file, and leave the task
`dispatched`. It reaches `review` after its last follow-up.

Then dispatch any queued task whose deps just completed, unless the project is
paused on a checkpoint.

## Restart

State files are the truth and reconcile is the recovery: a fresh orchestrator
session, or a fresh mux, resumes by running it. Completion events are files
too, so anything that finished while nothing was listening is delivered once
something is. Any usage.json `inflight` entry whose task has no result and no
live session is an orphan: fold it out and say so.

## Daily summary

On the first interaction of a calendar day (compare the newest log.jsonl
timestamp), open with one sentence from the last day's log — completions,
failures, quota spent — before handling the request. Skip it if the log is
empty.

## Digests

Speak only changes: finished, failed, newly stuck. One sentence per item, task
description not id. Nothing changed → say nothing (a notification you have
already reported) or "all quiet, N still running" (asked).

## Probes

For a worker running long with no result: fork its session read-only —
`mux --session <id> --fork "brief status: what are you doing and how far
along?"` — and relay a one-line summary. Log a `probe` line. Never probe more
than once per task per ~10 minutes.

## Stuck / failed

A worker the subsystem marked failed, or a probe describing a loop: tell the
user and ask whether to redispatch (same or different backend) — redispatch
costs quota, so it is the user's call.

## Checkpoints

When a checkpoint task completes: pause dispatching for that project, say
what's ready to test, and wait. The reply's final sentence must be a concise
test pointer: where the change lives and what to exercise. Feedback becomes new
or amended task records; then resume.
