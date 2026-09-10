# Orchestrator plan

A mux-first dispatcher: one dedicated mux session that stays free for
instruction, maintains per-project task lists, spawns worker sessions, watches
them, and gives short status digests. It rides mux's existing front ends —
terminal chat, Telegram, relay chat, and voice (local or over relay) — so it
must work fully by voice alone, but voice is one interface among several, not
the design. No dependence on any richer UI.

## What mux already provides

- **Voice front end**: the voice helper streams speech into the prompt box and
  reads replies aloud (`src/voice.c`); relay does the same from the phone
  (`src/relay.c`). The orchestrator session just runs with voice armed.
- **Spawning**: `mux [-b backend] [-m model] [-e effort] [-C cwd] "prompt"`
  runs a one-shot headless session (`src/main.c:712`). `mux_argv()`
  (`src/session.h:193`) builds that command line; the side channel and board
  already use it. `sessionfork_run()` (`src/sessionfork.c`) spawns into a tmux
  window so the user can jump in.
- **Tracking**: the live registry (`~/.config/mux/live`, `src/livelist.c`) has
  one JSON record per running session — backend, model, label, cwd, status,
  cost, tmux pane. Read-only, perfect for polling.
- **Persistent work records**: board cards (`~/.config/mux/board.jsonl`) carry
  tier, status, and accumulated cost, and survive session exit.
- **Cost**: per-session `cost_usd` from the `models.c` price table, rolled into
  board cards.

## Gaps

1. **Quota**: only the codex driver implements the `rate_limit` backend hook
   (`src/vendor/agents/backend.h:104,140`); claude.h has none. Until it does,
   quota awareness = cumulative cost + session counts per backend, tracked by
   the orchestrator itself.
2. **Board pump is not headless**: cards only advance while the board view is
   open (`src/boardview.c`). Either the orchestrator dispatches directly
   (bypassing the board), or the pump gets a headless mode — the latter is a
   general mux improvement.
3. **Completion signal**: one-shot sessions drop their live record on exit, so
   "gone from the live dir" is ambiguous (done vs. crashed). Workers need to
   write a result file on completion.
4. **Worker steering**: no channel injects input into a running session's
   turn — /btw forks are read-only, and a resumed turn (`mux --session <id>
   "prompt"`) only lands between turns. Mid-turn redirection ("stop, the
   requirement changed") needs either an inbox file the worker's harness
   checks between tool calls, or session abort + resume with the new
   instruction.

## Architecture

- **Orchestrator session**: a normal mux session, voice-armed, with an
  `orchestrate` skill loaded. It does no implementation work itself; every
  task is delegated so it stays responsive.
- **State** in `~/.config/orchestrator/`:
  - `projects/<name>.jsonl` — task list per project: id, description, status
    (queued / dispatched / done / failed), class, assigned backend+model,
    session id, cost.
  - `results/<task-id>.json` — written by the worker on completion (a footer
    injected via `--append-system-prompt` tells it to).
  - `registry.json` — project name → cwd (plus aliases), so "in mux, …"
    resolves to the right directory without the user ever saying a path.
  - `routing.json` — task class → backend/model:
    - planning: codex/astra, claude/fable
    - bug diagnosis: codex/astra, claude/fable
    - implementation (plans or fixes): claude/opus[1m], grok/grok-4.6,
      codex/sol
    - orchestrator session itself: claude/opus[1m] (for now)
    Plus per-backend budget ceilings.
  - `usage.json` — running cost and dispatch counts per backend, reset window.
- **Backend selection**: within a class, pick by *projected* quota, not the
  raw reading. The orchestrator learns a per-backend, per-class cost model:
  each completed task's observed quota delta (or cost, on backends without
  `rate_limit`) feeds a rolling estimate of what one task of that class
  costs on that backend. At dispatch time, projected remaining = last real
  reading − predicted cost of every in-flight task on that backend − the
  candidate task's predicted cost; dispatch to the backend whose projected
  remaining stays highest. Completion replaces the prediction with the
  observed spend and refines the estimate, so balancing gets fairer as data
  accumulates.
- **Dispatch**: workers live as sibling sessions inside the orchestrator's
  own mux instance — the session menu's "new session" path:
  `workspace_spawn(backend, model, cwd)` + `workspace_send(at, prompt)`
  (`src/sessionswitch.c:400-419`; `ask_new` at sessionswitch.c:496 already
  spawns without stealing focus). They appear in the session menu and are
  switchable like any tab. The board workers already spawn workspace
  sessions this way (`src/boardwork.c`). **Built**: `src/dispatch.c` — the
  agent drops `~/.config/mux/dispatch/$MUX_PID-<id>.req` (JSON: backend,
  model, effort, cwd, prompt; all optional) and the main loop spawns the
  sibling session without stealing focus, replying in `…-<id>.res` with the
  slot and session id. mux exports `MUX_PID` to its children. Records the
  mapping in the project file.
- **Monitoring**: a self-paced `/loop` in the orchestrator session. Each tick:
  scan live records for its dispatched session ids, pick up new result files,
  update task state and usage, and speak a one-sentence digest only when
  something changed (finished, failed, stuck > N minutes).
- **Progress probes**: for a long-running or stuck worker, fork its session
  mid-turn — the /btw mechanism (`sidechannel_start`, `src/sidechannel.c:272`,
  i.e. `mux --session <id> --fork "brief status: what are you doing and how
  far along?"`). The fork sees the worker's full context and answers without
  disturbing the live turn; the orchestrator relays a one-line summary aloud.
- **User check-ins**: tasks (or batches) can carry a checkpoint. When one is
  reached, dispatching for that project pauses, the orchestrator speaks what's
  ready to test, and waits; spoken feedback is turned into new or amended
  tasks before dispatch resumes. Also a periodic fallback checkpoint (e.g.
  every N completed tasks) so long autonomous runs never drift too far
  without a review.
- **Voice grammar** (skill-defined, all answers 1–2 sentences for TTS):
  add a task to a project, list a project's tasks, dispatch / dispatch all,
  status, reassign a task to a different model, pause/resume dispatching,
  kill a task, read usage.

## Phases

1. **Task lists + voice admin** — skill, state dir, project registry,
   per-project JSONL; add / list / status by voice. No dispatch yet. Usable
   on day one.
2. **Dispatch + completion** — spawn workers with the injected result-file
   footer; track via live dir + results; report done/failed on the next tick.
3. **Monitoring loop** — self-paced wakeups, spoken digests, stuck detection,
   /btw progress probes, user checkpoints, jump-to-session on request
   ("show me the auth task").
4. **Routing + quota** — routing.json classes, per-backend budgets from tracked
   cost; optionally implement the `rate_limit` hook in claude.h so real quota
   replaces the cost proxy.
5. **Hardening** — orchestrator restart reattaches to in-flight tasks from
   state; dedupe; reopen crashed workers; daily spoken summary.

## Task breakdown

What the orchestrator's own intake step would produce from this blueprint,
grouped by class. **planning** → codex/astra, claude/fable (also bug
diagnosis, which doesn't appear in this greenfield breakdown); **impl** →
claude/opus[1m], grok/grok-4.6, codex/sol. Phase in parens gives ordering.

Planning:
1. State dir layout + JSONL schemas for tasks, results, registry, routing,
   usage. Schema decisions ripple through everything. *(phase 1)*
   **Done — see docs/orchestrator-schemas.md.**

Implementation:
2. Project registry file + name/alias resolution. *(phase 1)*
3. `orchestrate` skill v1: add / list / status / complete via any front end,
   answers sized for TTS. Structured firstmate-style: a thin always-loaded
   contract (role, boundaries, routing index) that lazily loads internal
   sub-skills at trigger points, not one monolithic prompt. *(phase 1)*
   **Done — ~/.claude/skills/orchestrate (contract + tasks/intake/dispatch/
   monitor references); routing.json and usage.json seeded.**
4. Intake command: "here's a plan for X" → decomposed, classified tasks;
   the decomposition itself runs on a planning-class model. *(phase 1)*
5. Dispatch: class → backend/model via quota-balanced selection, spawn the
   worker as a sibling session in the orchestrator's mux instance
   (workspace_spawn/workspace_send; needs an agent-callable mux command),
   in a per-task worktree, record session id. *(phase 2)*
6. Result-file footer via `--append-system-prompt` + result schema.
   *(phase 2)*
7. Reconcile: live-dir scan + result pickup → task state + usage. *(phase 2)*
8. fswatch watcher + fallback heartbeat loop. *(phase 3)*
9. /btw progress probe for long/stuck workers. *(phase 3)*
10. Checkpoint pauses + spoken digest rules. *(phase 3)*
11. Jump-to-session by task name. *(phase 3)*
12. routing.json + quota-balanced backend selection + per-backend budgets
    from tracked cost. *(phase 4)*
13. `rate_limit` hook in claude.h — vendor code, plan it first. *(phase 4)*
14. Worker follow-up channel: send a new turn to an idle worker via session
    resume; mid-turn steering via inbox file or abort-and-resume (gap 4).
    *(phase 3)*
15. Restart reconcile, dedupe, crashed-worker reopen, daily summary.
    *(phase 5)*

## Prior art: firstmate (github.com/kunchenguid/firstmate)

Same supervisor-plus-crew shape: one chat-facing agent spawns isolated
workers, all state on disk so a restart reconciles and continues. Ideas worth
adopting:

- **Event-driven supervision** instead of pure polling: a cheap watcher
  (fswatch on the live dir + results dir) wakes the orchestrator only when a
  record changes, with the self-paced loop kept as a long fallback heartbeat.
- **Per-task git worktrees** for workers, so parallel tasks on one repo never
  collide (matches the existing `.claude/worktrees/<name>` convention).

Where the plans differ: firstmate is harness-portable; this is mux-first —
every mux front end (terminal, Telegram, relay, voice) for free — and leans
into mux internals for things firstmate can't do —
mid-turn /btw progress probes, the live registry, per-session cost — and it
plans quota/budget routing, which firstmate doesn't address.

## Open questions

- Reuse the board (and make its pump headless) vs. orchestrator-private
  dispatch. Private dispatch is less code now; headless board helps all of mux.
- Whether the orchestrator should also drain `~/.config/mux/reminders` (today
  only the Telegram path drains them).
