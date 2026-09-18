# Orchestrator state: layout and schemas

All state lives in `~/.config/orchestrator/`. Files are JSON or JSONL
(append-only, one object per line, newest record per id wins — same
convention as mux board.jsonl).
Timestamps are unix epoch seconds. Persistent ids are short random hex,
prefixed by kind (`t-`, `r-`). Transient one-off request keys use `o-`.

## Layout

    ~/.config/orchestrator/
      registry.json        project name → cwd
      routing.json         class → candidate backends, budgets
      usage.json           per-backend quota estimate
      projects/<name>.jsonl  task log per project
      results/<task-id>.json result written by a project worker
      results/<one-off-key>.json transient result written by a one-off subagent
      events/              completion events waiting to be delivered, written by
                           the instance hosting a worker and drained by the
                           instance running the orchestrator
      requests/            `mux orch` request and reply files
      log.jsonl            project dispatch/reconcile log (debugging, digests)

Everything here is written by the mux orchestrator subsystem, under a lock per
file. Read them freely; write them through `mux orch`.

## registry.json

    {
      "projects": [
        {
          "name": "mux",
          "cwd": "/Users/jhickner/working/apps/mux",
          "aliases": ["the repl"]
        }
      ]
    }

Name resolution: exact name, then alias, case-insensitive.

## Task record (projects/<name>.jsonl)

    {
      "id": "t-3fa2c1",
      "desc": "implement the project registry + name resolution",
      "class": "planning" | "diagnosis" | "impl",
      "status": "queued" | "dispatched" | "review" | "done" | "failed" |
                "paused" | "cancelled",
      "created": 1757520000,
      "updated": 1757523600,
      "deps": ["t-9b01aa"],
      "checkpoint": false,
      "backend": "claude",        // set at dispatch
      "model": "opus[1m]",
      "session": "<mux session id>",
      "pid": 12345,                 // the instance the work belongs to: set at
                                    // creation as the origin, and again at
                                    // dispatch as the host
      "worktree": ".claude/worktrees/t-3fa2c1",
      "cost_usd": 0.42,
      "pending": ["follow-up tweak for the same worker"],  // optional
      "notes": "free text: feedback, reassignments"
    }

A status change appends a new full record with the same id. `review` means
the worker finished and the change awaits user review and merge. `done` means
complete and merged, and is terminal: nothing moves out of it. `cancelled`
means abandoned; it leaves the open list like `done` but can be reopened.
`paused` marks a checkpoint hold. `deps` gates dispatch on other tasks being
`done`. `pending` holds follow-up instructions queued for the task's live
worker (tasks.md, Follow-ups).

## Result file (results/<task-id>.json)

Written by the worker as its final act; the injected system-prompt footer
specifies path and shape. Presence of the file is the completion signal.

    {
      "task": "t-3fa2c1",
      "status": "done" | "failed",
      "summary": "one paragraph, sized for TTS",
      "files": ["src/registry.c"],
      "commit": "abc1234",          // optional
      "followups": ["suggested new task descriptions"],  // optional
      "finished": 1757523600
    }

## One-off result (results/<one-off-key>.json)

A one-off subagent writes this as its final act. It is a transient completion
signal, not a project task record; read and delete it without appending a task
or task lifecycle event.

    {
      "one_off": "o-4bc921",
      "status": "done" | "failed",
      "summary": "the requested finding, sized for TTS",
      "finished": 1757523600
    }

## Completion event (events/<name>.json)

Written by the instance hosting a worker when that worker finishes a turn or
ends, and deleted by the orchestrator once it has been delivered. Not a
completion in itself: the result file is still the signal, and a lost event
costs nothing but time, since reconcile finds the same thing.

    {
      "task": "t-3fa2c1",
      "session": "<the worker's session id>",
      "notify": "<who asked to be told>",
      "reason": "idle" | "exit",
      "ts": 1757523600
    }

## routing.json

    {
      "classes": {
        "planning":  [{"backend": "codex", "model": "astra"},
                      {"backend": "claude", "model": "fable"}],
        "diagnosis": [{"backend": "codex", "model": "astra"},
                      {"backend": "claude", "model": "fable"}],
        "impl":      [{"backend": "claude", "model": "opus[1m]"},
                      {"backend": "grok", "model": "grok-4.6"},
                      {"backend": "codex", "model": "sol"}]
      },
      "orchestrator": {"backend": "claude", "model": "opus[1m]"},
      "seed_cost": {"planning": 3, "diagnosis": 2, "impl": 5},
      "budgets": {"claude": {"max_pct_per_day": 40}}
    }

Candidates within a class are equivalent; selection is quota-balanced (see
usage.json). `seed_cost` is only the prior for a (backend, class) pair with
no observations yet; learned estimates in usage.json take over from there.

## usage.json

    {
      "backends": {
        "claude": {
          "source": "rate_limit" | "cost",
          "remaining_pct": 51,        // last real reading, if any
          "read_at": 1757520000,
          "cost_usd": 3.10,
          "dispatches": 4,
          "resets_at": 1757534400,
          "task_cost": {              // learned pct-per-task, by class
            "impl":     {"est": 4.2, "n": 11},
            "planning": {"est": 2.8, "n": 3}
          },
          "inflight": [               // predicted spend not yet observed
            {"task": "t-3fa2c1", "class": "impl", "est": 4.2}
          ]
        },
        "grok": { ... },
        "codex": { ... }
      }
    }

Selection: projected = `remaining_pct` − Σ inflight `est` − candidate's
`est` (from `task_cost`, falling back to routing `seed_cost` when `n` is 0);
dispatch to the backend with the highest projected value. On dispatch the
prediction joins `inflight`; on completion it is removed and the observed
quota delta (or cost-derived equivalent) updates `task_cost.est` as an
exponential moving average and increments `n`. A fresh `rate_limit` reading
re-anchors `remaining_pct` and clears any drift between predicted and real.

## log.jsonl

    {"at": 1757520000, "ev": "dispatch", "task": "t-3fa2c1",
     "backend": "claude", "detail": "..."}

Events: `dispatch`, `result`, `probe`, `stuck`, `checkpoint`, `reassign`,
`error`. Source for spoken digests and the daily summary.
