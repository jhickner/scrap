# Orchestrator as a mux subsystem — staged implementation plan

Checkpoint deliverable for task t-7c32b5. No production code lands from this
task. Each stage below is buildable, testable and reviewable on its own, and
the stages are listed in dependency order.

## Verified grounding

Checked against the worktree at commit 452ac91:

- `src/filelock.h` exposes `filelock_acquire(path, op)` over a `<path>.lock`
  file and `filelock_release(fd)`. Owner election is
  `filelock_acquire(path, LOCK_EX | LOCK_NB)` on
  `/tmp/mux-<uid>-<subsystem>`, at `src/relay.c:112` (`claim_relay`) and
  `src/tg.c:177` (`claim_telegram`). Both store the fd in a file-scope
  `owner_lock`, write a `start_error[256]`, print `mux: <error>` to stderr,
  and return 0. The contention wording is
  `"relay is already enabled by another mux instance"`; the other branch is
  `"could not claim relay for this mux instance: %s"`.
- `MUX_PID` is set once at `src/main.c:716` from `getpid()`, before
  `ui_init()`. A `setenv` sweep of `src/*.c` finds nothing per-session:
  `agenttabs.c:159`, `bash.c:358-363`, `tty.c:321`, `viewport.c:1611`.
- `~/.config/mux/live/<pid>-<slot>.json` records pid, slot, backend, model,
  label, effort, cwd, id, parent, title, status, card, unseen, tmux
  pane/window and ts (`src/livelist.h`). `livelist_load`,
  `livelist_closed_load` and `livelist_alive(pid)` already exist.
- `src/dispatch.c` implements the request protocol: `<pid>-<key>.req` in
  `~/.config/mux/dispatch` (overridable by `MUX_DISPATCH_DIR`), replies at
  `<pid>-<key>.res` written through a `.res.tmp` rename. Spawns whose id is
  not yet known are parked in `pendings[]` and settled by `settle_pending`
  with a 30s `ID_WAIT_MS` cap.
- `src/orchstatus.c:211` drops a task only when `status` equals `"done"`.
- The embedded skill lives in `orchestrate/` (SKILL.md, references/, quota.sh,
  routing.json), is generated into `src/orchdata.c` by `tools/gen-embed.sh`
  via the Makefile, and is written out at startup by `orch_install()`
  (`src/orchinstall.c`, called from `src/main.c:706`).

Two grounding corrections, both load-bearing:

1. **`MAX_SLOTS 32` is the wrong capacity limit.** `src/agenttabs.c:13`
   bounds the agent-status slot cache, not sessions. The session limit is
   `WORKSPACE_MAX 12` in `src/workspace.h:6`. Free-slot arithmetic in stage 6
   must count live records per pid against 12.
2. **No per-child environment hook exists.** The vendored backends fork and
   exec directly (`src/vendor/agents/claude/claude.h:476` and `:1059`), so a
   child inherits mux's process environment as-is. Exporting a per-session
   variable means bracketing the synchronous `session_start` call with
   `setenv`/`unsetenv`, or adding an env hook to the vendored headers. See
   stage 2.

## Design issues to settle before stage 6 and stage 7

These are places where the settled design is wrong or thin. I have not worked
around them silently.

- **"A worker cannot outlive the mux hosting it" is true per instance, not
  per orchestrator.** The claiming instance can exit while a second instance
  still hosts live workers. With no re-claim (accepted), those workers finish
  with no orchestrator listening. The completion queue therefore has to be
  durable on disk and drained at orchestrator start; it cannot be an in-memory
  fan-out. The plan assumes that.
- **Notify delivery is cross-instance.** The session named by `notify` may
  live in a different instance from the worker. A raw `workspace_send` only
  reaches sessions in the same process. Delivery must either go through the
  instance-addressed dispatch request file (`<pid>-<key>.req`, so the notify
  record must carry the orchestrator's pid) or be pulled by the orchestrator
  subsystem from the queue. I recommend the pull model: the worker's instance
  only appends an event, and the orchestrator subsystem in whatever instance
  owns the lock drains and delivers it. That removes the cross-instance send
  entirely.
- **"Goes idle" is not "finished".** `src/session.c:404` publishes
  `finished` at the end of *every* turn, including the first turn of a
  multi-turn worker and every follow-up. Firing notify on each idle
  transition produces repeated completion events per task. The design does
  not say which idle counts. Recommendation: fire on idle only when the
  session has no turn queued and either a result file exists for the task or
  the session has since exited, and mark the notify registration one-shot per
  task with a `fired` flag in the event record. Follow-ups then re-arm it
  explicitly. This needs the user's call.
- **The CLI surface should not require the claiming instance.** Item 5 as
  written makes every terminal operation depend on a live owner. State files
  are the source of truth, so reads and pure state transitions can run
  in-process in the CLI under the same file lock that the subsystem uses.
  Recommendation: implement every task operation in a shared library (stage
  3), let the CLI call it directly, and route through the subsystem only for
  operations that need a live mux: spawn, send, close, targeting. This keeps
  `mux orch list` working with no mux running at all.
- **`/tasks` hides only `done` today, which also leaves `failed` and `paused`
  showing as open.** Stage 1 adds `cancelled`. Whether `failed` should also
  drop out of the default view is a separate product call; I have kept it
  visible and flagged it.
- **The 30s spawn id wait is a hard dependency of targeting.** If the backend
  never reports an id, the dispatch is unaddressable and must be recorded as
  failed rather than left `dispatched`, otherwise reconcile sees a task with
  a session that never existed.

## Stage 1 — cancelled status (scope item 7)

Smallest independent piece, no dependencies, ships first so the rest of the
work has a terminal status to use for abandoned dispatches.

- **Changes.** Add `cancelled` to the task status vocabulary. Filter it
  alongside `done` in the default `/tasks` view.
- **Files.** `src/orchstatus.c` (the filter at line 211, plus any status
  colouring in `orchstatus_status`), `orchestrate/references/schemas.md` and
  `orchestrate/references/tasks.md` for the status enum and the transition
  that produces it.
- **Tests.** `tests/orchstatustest.c`: a project file with one task per
  status, asserting `include_done = 0` keeps queued, dispatched, review,
  failed and paused, and drops done and cancelled; `include_done = 1` keeps
  all of them.
- **Risk.** Low. The only breakage is a status string mismatch between the
  C filter and what the skill writes; the test pins both ends.

## Stage 2 — session self-address (scope item 1)

- **Changes.** At spawn, mux allocates a per-session address file and exports
  its path as `MUX_SESSION_FILE` for that child only. When the backend
  reports its session id, mux writes the id into that file through a
  tmp+rename. A session can then read its own id with
  `cat "$MUX_SESSION_FILE"`.
- **Files.** New `src/sessionaddr.c/.h` owning path allocation, the atomic
  write and cleanup on session close. `src/session.c` calls it at the two
  points where `s->id` is first filled (`session.c:580` when the backend
  reports an id, and `session_adopt_id` around line 1155). `src/dispatch.c`
  allocates the address file before `workspace_spawn` and includes its path
  in the spawn reply. `src/workspace.c` for the spawn-time env bracket and
  the close-time unlink.
- **Environment mechanics.** `session_start` forks synchronously, so
  `setenv("MUX_SESSION_FILE", path, 1)` immediately before it and `unsetenv`
  immediately after gives the child the value without leaking it to later
  spawns. Caveat: mux runs relay and telegram server threads, and
  `setenv`/`fork` in a multithreaded process is only safe because no other
  thread mutates the environment. If that is judged too sharp, the
  alternative is an explicit env-pair hook in the vendored agent headers,
  which is a larger and more invasive change; I recommend the bracket, with a
  comment at the call site.
- **Tests.** New `tests/sessionaddrtest.c` (add to `CHECKS` in the Makefile,
  which errors if a test file is unlisted): path allocation is unique per
  session, the write is atomic and readable, a late id overwrites an empty
  file, and close unlinks. Extend `tests/dispatchtest.c` to assert the spawn
  reply carries the address path and that the file holds the id the stub
  backend reported.
- **Risk.** A stale address file if mux is killed; harmless, but the
  allocator should use `<pid>-<slot>` naming so a restart reuses the slot
  rather than accumulating files. Env leakage into unrelated children (bash
  tool calls) if the bracket is wrong — covered by asserting `getenv` is
  clear after spawn returns.

## Stage 3 — task state library (scope item 4)

Pure library, no subsystem yet. Everything later builds on it.

- **Changes.** New module owning `~/.config/orchestrator`: read
  `registry.json`, read and append `projects/<name>.jsonl` (append-only
  preserved, newest record per id wins), read and delete
  `results/<id>.json`, append `log.jsonl`. Operations: list open, list all,
  inspect one, create, add follow-up, and the transitions dispatched,
  review, done, failed, cancelled. Every write is tmp+rename under
  `filelock_acquire(path, LOCK_EX)` so the CLI and the subsystem cannot
  interleave. A directory override env var (`ORCHESTRATOR_DIR`, matching the
  `MUX_DISPATCH_DIR` and `AGENT_TABS_STATE_DIR` precedent) makes it testable.
- **Files.** New `src/orchtask.c/.h`. `src/orchstatus.c` keeps its own
  read-only loader for `/tasks`; consider folding it into the new module in a
  later cleanup rather than now, so this stage does not perturb a working
  view.
- **Output.** Two renderers: a stable human table and a JSON form. Both are
  the same field set, so the CLI and the subsystem never diverge.
- **Tests.** New `tests/orchtasktest.c`: append-then-read returns the newest
  record; a partial write is never observed (write a record, kill the
  rename, assert the previous state survives); concurrent appends under the
  lock do not interleave lines; unknown project name resolves by alias then
  fails cleanly; transitions reject illegal moves (done to queued).
- **Risk.** The skill currently writes these files by hand and will keep
  doing so until stage 8. The two writers must agree on field order
  tolerance (they do, since JSONL is order-insensitive) and on the lock.
  Until stage 8 lands, the skill does not take the lock, so the window for a
  torn read exists; mitigate by making all library reads tolerant of a
  trailing partial line, which is already the JSONL convention.

## Stage 4 — subsystem skeleton and lifecycle

- **Changes.** New subsystem started exactly like relay and telegram: an
  `--orchestrator` flag, a `/orchestrator [on|off]` command, single-instance
  election through `filelock_acquire("/tmp/mux-<uid>-orchestrator",
  LOCK_EX | LOCK_NB)`, contention message
  `"orchestrator is already enabled by another mux instance"` and the
  matching `could not claim` branch, both to stderr prefixed `mux: `. No
  handoff, no periodic re-claim — documented as inherited behaviour.
  Includes the reconcile timer (scope item 3): an internal tick, default 20
  minutes, from a `orchestrator_reconcile_minutes` setting in
  `src/settings.h`, doing nothing yet beyond firing a no-op hook.
- **Files.** New `src/orch.c/.h` mirroring `src/relay.h`'s shape
  (`orch_start`, `orch_stop`, `orch_start_error`, `orch_label`, `orch_poll`).
  `src/main.c`: long option near line 564, the flag variables near 580, the
  start call beside `relay_start` near 782, `orch_stop` near 1012, the usage
  text near 126, and `orch_poll` in `idle_render` beside `dispatch_poll`.
  `src/cmd.c`: `do_orchestrator` beside `do_relay` at line 613 and the table
  entry beside `/relay` at line 1401. `src/settings.h` for the interval key.
- **Tests.** New `tests/orchtest.c`: a second claim on the same lock path
  fails with the exact contention wording; release re-allows a claim; the
  timer fires on schedule against an injected clock. `tests/filelocktest.c`
  already covers the primitive.
- **Risk.** `--orchestrator` combined with a non-interactive prompt should be
  refused the way `--telegram` and `--relay` are at `src/main.c:725`.
  Forgetting `orch_stop` in the exit path leaks the lock until process exit,
  which flock releases anyway, so the failure mode is mild.

## Stage 5 — CLI surface (scope item 5)

- **Changes.** A `mux orch <verb>` entry point reachable from any terminal.
  Reads and pure state transitions run in-process against the stage 3
  library under its lock, so they work with no mux running. Verbs that need
  a live instance (dispatch, send, close) write a request file and wait for
  the reply, mirroring the dispatch directory protocol, and fail with a clear
  message when no claiming instance is alive.
- **Files.** `src/main.c` argv dispatch before the interactive path; new
  `src/orchcli.c/.h`; `src/orch.c` grows the request-file server half.
- **Tests.** New `tests/orchclitest.c`: each read verb produces stable human
  and JSON output against a fixture directory; a live-instance verb with no
  owner exits non-zero with the documented message; a request/reply round
  trip against a stub server; malformed request files are rejected, not
  crashed on.
- **Risk.** Argv parsing regression for existing invocations — `mux orch`
  must not shadow a prompt that happens to begin with the word orch. Gate on
  the exact first argument and only when more arguments follow.

## Stage 6 — dispatch targeting (scope item 6)

- **Changes.** Enumerate instances from `~/.config/mux/live`, liveness-check
  each pid with `livelist_alive`, count that pid's live sessions against
  `WORKSPACE_MAX` (12, not 32), and choose: the task's origin pid first,
  else the most recently active instance by `ts` with a free slot, else
  leave the task queued. Record the requesting session's pid on the task at
  creation so origin preference has something to read.
- **Files.** New `src/orchtarget.c/.h`; `src/orchtask.c` for the origin pid
  field; `orchestrate/references/schemas.md` for the field.
- **Tests.** New `tests/orchtargettest.c` over a fixture live directory:
  origin pid wins when alive and under capacity; a full instance is skipped;
  a dead pid is skipped even with recent records; no candidate leaves the
  task queued rather than dispatching anywhere.
- **Risk.** Stale live records for a crashed instance. `livelist_alive`
  handles the pid check, but pid reuse could resurrect a dead instance;
  compare the record ts against the process start time if that proves real.
  Duplicate dispatch — the failure that produced two workers on t-3e8c15 —
  is prevented here by refusing to dispatch a task already in `dispatched`
  with a live session, which must be an explicit assertion in this stage,
  not an emergent property.

## Stage 7 — completion notification (scope item 2)

The largest stage and the one with the open design questions above. Split in
two so the mechanism can be reviewed before the policy.

### 7a — durable event queue and emission

- **Changes.** A `notify` field on the spawn request naming the session to
  inform. The worker's instance appends a durable event record (task,
  session, reason `exit` or `idle`, timestamp, delivered flag) through
  tmp+rename into `~/.config/orchestrator/events/`. Emission points:
  `tab_busy` in `src/session.c:404` for the idle transition, and the session
  close and process-exit paths for the lost-worker case, which is the
  currently silent failure.
- **Files.** `src/dispatch.c` (accept and record `notify`), `src/session.c`
  (emission hooks), new `src/orchevent.c/.h` (append, list, ack, prune).
- **Tests.** New `tests/orcheventtest.c`: an event survives a simulated
  restart; acking removes it exactly once; a duplicate emission for the same
  task and reason collapses to one undelivered event; a worker killed
  without writing a result produces an exit event. Extend
  `tests/dispatchtest.c` for the `notify` field.

### 7b — delivery, drain and reconcile

- **Changes.** The orchestrator subsystem drains undelivered events on start
  and on every poll, delivers each as a line into the orchestrator session
  through the existing send path, and acks only after the send is accepted.
  The reconcile timer from stage 4 gains its real body: scan results,
  reconcile against live sessions, fold in anything a missed event dropped.
  A missed notification degrades to found-on-next-reconcile; result files
  stay the completion signal and notifications stay a nudge.
- **Files.** `src/orch.c`, `src/orchevent.c`, `src/orchtask.c`.
- **Tests.** New `tests/orchdrain.c` cases for the five scenarios the design
  names: idle completion delivered once; restart drains a backlog; a
  duplicate event delivers once; a worker that died with no result file is
  reported as lost; two simultaneous completions both deliver and both ack.
  Plus the negative case that matters most — delivery failure leaves the
  event unacked and it is retried.
- **Risk.** Delivering into a session mid-turn queues the line, which is
  correct but means ack-on-accept is not ack-on-read; the event record must
  survive until the orchestrator turn actually consumes it, or an
  orchestrator crash between accept and consumption loses the nudge. That is
  tolerable because reconcile is the guarantee, but it should be stated in
  the code comment rather than discovered.

## Stage 8 — skill rewrite (scope item 8)

- **Changes.** Shrink the skill to intake, judgement and conversation. Drop
  the background-waiter instruction from `references/dispatch.md`. Replace
  hand-written JSONL manipulation, the reconcile procedure, the quota EMA
  arithmetic and the dedupe scan with calls to the CLI from stage 5. Keep
  routing judgement and the spoken-reply discipline in the skill.
- **Files.** `orchestrate/SKILL.md`, `orchestrate/references/*.md`.
  `src/orchdata.c` regenerates from the Makefile rule automatically.
  `tests/orchinstalltest.c` may need its fixture list updated if reference
  files are removed.
- **Tests.** `make check` for the install test; a manual pass driving the
  skill against a scratch `ORCHESTRATOR_DIR`.
- **Risk.** The highest-consequence stage for behaviour even though it is
  only prose. Removing a reference file that the installed copy still has on
  disk leaves a stale file in `~/.claude/skills/orchestrate`;
  `src/orchinstall.c` writes but does not prune, so this stage needs either
  a prune pass or an explicit note that stale references must be deleted by
  hand.

## Order and review points

Stages 1, 2 and 3 are independent of each other and can be built in any
order or in parallel. Stage 4 depends on 3. Stage 5 depends on 3 and 4.
Stage 6 depends on 3 and 5. Stage 7a depends on 2 and 4; 7b depends on 7a, 3
and 6. Stage 8 depends on 5, 6 and 7.

Natural review points: after stage 3 (state library correct in isolation),
after stage 5 (the CLI is usable by hand before anything automatic depends
on it), and after stage 7a (the event mechanism is reviewable before the
delivery policy commits to an answer on the idle-versus-finished question).

---

# What landed

The checkpoint plan above was reviewed and the user asked for the
implementation in the same pass, so all eight stages are built in this
worktree. Nothing is committed. `make check` passes, including five new
harnesses.

## Stage by stage

| Stage | State | Where |
| --- | --- | --- |
| 1 cancelled status | done | `src/orchstatus.c`, `orchstatus_closed` |
| 2 session self-address | done | `src/sessionaddr.c`, the five agent headers |
| 3 task state library | done | `src/orchtask.c` |
| 4 subsystem and lifecycle | done | `src/orch.c`, `--orchestrator`, `/orchestrator` |
| 5 command line | done | `src/orchcli.c`, `mux orch` |
| 6 dispatch targeting | done | `src/orchtarget.c` |
| 7 completion notification | done | `src/orchevent.c`, watches in `src/dispatch.c` |
| 8 skill rewrite | done | `orchestrate/` |

New tests, all in `CHECKS`: `sessionaddrtest`, `orchtasktest`,
`orchtargettest`, `orcheventtest`, `orchclitest`, `orchtest`. `dispatchtest`
and `orchstatustest` grew cases.

## Where the implementation differs from the plan

- **Per-child environment.** The bracketed `setenv` around the spawn was
  rejected once the restart path turned out to fork on a background thread.
  Instead `session_file` became a first-class backend option: it is set in
  `backend_opts`, carried in `backend_state`, and applied in the child after
  the fork and before the exec in each of the five drivers. No global
  environment mutation, no race with a concurrent start.
- **Every session gets an address file, not just dispatched ones.**
  `session_new` allocates it, so any session can name itself, and the spawn
  reply reports the path as `addr` for the caller that wants it.
- **The command line does its own reads and writes.** As recommended, list,
  show, add, status, note and followup run in process under the same lock the
  subsystem uses, so they work with no mux running. Only dispatch, send, tell
  and reconcile need the owner, and they say so plainly when there is none.
- **Requests are addressed to whoever owns the orchestrator.** A caller cannot
  know the owning pid, so it writes `any-<key>.req` and the owner renames it to
  itself. The reply keeps the key, so the caller finds it by its tail.
- **Delivery is pull, not push.** The worker's instance only records an event;
  the instance holding the lock delivers it. That removes the cross-instance
  send the plan flagged as unworkable.
- **Idle is disambiguated.** A watch fires once, on the first turn that starts
  and then ends, and a watch is dropped when it fires. A later follow-up does
  not re-report the task. The open question from the plan is answered this way;
  it is one line in `watch_poll` to change if it reads wrong in use.

## Two bugs the tests caught

- **Appending after a torn line joined two records.** A project file whose last
  line was written by a process that died mid-write has no terminating newline.
  Appending straight onto it produced one unparseable line and lost both
  records. `append_line` now closes the torn line first, under the lock.
- **`serve_dispatch` read a task's status and session after freeing them.** The
  duplicate-dispatch guard compared pointers into a deleted cJSON tree, so the
  guard silently never fired: exactly the two-workers-on-one-task failure it
  exists to prevent. The fields are copied out before the tree is freed.

## Still open

- **No owner handoff**, as decided. The orchestrator stops when its instance
  exits. Events written meanwhile are durable, so the next owner drains them.
- **`mux orch` shadows a prompt beginning with the word orch.** `mux orch` is
  taken as the orchestrator command line whenever it is the first argument.
- **Quota accounting stays in the skill.** `usage.json`, the exponential moving
  average and the backend choice were left where they were; only the task
  bookkeeping moved into code. Moving quota too is a reasonable follow-up.
- **The live path is untested end to end.** The subsystem's logic is covered
  headless, but starting mux with `--orchestrator` and watching a real worker
  finish needs a terminal and a real backend. That is the thing to exercise by
  hand first.
