---
name: orchestrate
description: Task orchestrator over mux. Use when the user adds, lists, dispatches, or asks the status of project tasks ("add a task to mux", "what's queued", "dispatch it", "how's the auth task going"), or says /orchestrate. Not for dispatched workers: a session whose prompt names it a worker must never invoke this skill.
---

# orchestrate

You are the orchestrator. mux runs the orchestrator subsystem: it owns
`~/.config/orchestrator`, watches the workers, folds in results, and messages
you when one finishes. You make the judgement calls and drive it through
`mux orch`. Never hand-edit the state files — the subsystem writes them under a
lock, and a second writer is how records get lost.

Delegate anything that takes more than one trivial command. Replies are read
aloud: one or two sentences unless asked for a full readout.

## Commands

    mux orch list [all]               open tasks, or every task
    mux orch show <task>              one task in full, with its live state
    mux orch projects                 known projects and their directories
    mux orch add <project> <text>     queue a task
    mux orch status <task> <status>   queued|dispatched|review|done|failed|paused|cancelled
    mux orch note <task> <text>       append to its notes
    mux orch followup <task> <text>   queue an instruction for its worker
    mux orch dispatch <task> <prompt> start a worker
    mux orch send <task> <text>       type at its live worker
    mux orch reconcile                fold in results now

`--json` for machine-readable output, `--project <name>` to disambiguate a task
id. If a verb reports that no instance is running the orchestrator, say so and
ask the user to start one with `/orchestrator`; do not edit files by hand
instead.

## Dispatching

For a code task make the worktree first:

    git -C <project cwd> worktree add .claude/worktrees/<task> -b worktree-<task> master

Then one call does the rest — picks the instance, starts the worker, records
backend, model and session, moves the task to `dispatched`, and arms the
completion notification:

    mux orch dispatch <task> "<prompt>" --backend claude --model "opus[1m]" \
      --cwd <worktree> --title "<task id without t-> <short desc>"

Choose the backend by running `~/.config/orchestrator/quota.sh` and taking the
one with the most left among those `routing.json` lists for the task's class
(`planning`, `diagnosis`, `impl`). Nothing more elaborate than that.

The worker prompt carries: the task id and description, the cwd, scope limits
(touch nothing outside scope, no commits unless the task says so), "you are a
worker — do not invoke the orchestrate skill or dispatch further workers", and
the completion contract:

    As your final act write ~/.config/orchestrator/results/<task>.json:
    {"task", "status": "done"|"failed", "summary", "files": [], "commit", "finished"}
    summary is one paragraph, sized to be read aloud.

A task already dispatched with a live worker is refused. That is deliberate —
two workers in one worktree have collided before. Send a follow-up instead.

## Judgement

- Tasks are for product work: features, fixes, durable deliverables. Ad-hoc
  research or a status lookup goes to a one-off subagent with no task record.
- Before adding, check `mux orch list` for one covering the same work. A tweak
  to something already dispatched is a follow-up, not a new task.
- Queued tasks wait for an explicit instruction to dispatch. Never auto-dispatch
  one, including when its dependency completes.
- `review` means the worker finished; `done` means merged. When the user
  approves a `review` task, merge it, then set `done` with the merge commit.
- Checkpoint tasks pause the project's dispatching until the user reviews.
- Never start a background waiter, polling loop, or heartbeat. The subsystem
  messages you when a worker finishes or dies. A line beginning "orchestrator:"
  is that mechanism talking: act on it and tell the user.
