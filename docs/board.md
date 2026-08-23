# The board

A card is a thought you had. The board is what happens to it.

Capture is meant to cost nothing: `/card fix the thing with the tabs`, or `mux
--card` from anywhere, appends a line and returns. No agent runs, nothing is
classified, you keep typing. Everything after that — working out what the card
meant, which repo it belongs to, who should build it, whether it is worth a
worker at all — happens without you, and comes back to you only at the points
where your judgement is the thing that is actually needed.

## The flow

```
new → unclear → backlog → doing → review → [audit] → merging → done
        ↑__________|         ↑_______|________|_________|
```

Triage moves a card out of `new`. Work kinds land in `backlog` and wait for a
worker; wiki kinds are filed and go straight to `done`; anything the classifier
could not parse goes to `unclear` and waits for a sentence from you.

A worker takes a card from `backlog`, works in a worktree of its own, and puts
it in `review` when it has something testable. You approve, reject, or send
feedback. Approval optionally routes through an `audit` — an LLM pass over the
diff — and then through `merging`, which is serialized, because parallel
workers branched off the same base do not land cleanly on their own.

Every arrow back to `doing` carries the reason with it, in the card's log, so
whoever picks the card up next reads why it bounced.

## Cards

One store, at `~/.config/mux/board.jsonl`, one card per line, written whole
under an exclusive lock — the same shape `reminders.c` uses. It is machine-wide
rather than per-repo: `cwd` is a field on the card, and the board filters by it,
defaulting to the directory you opened it from. A card thrown from the phone
does not know which repo it belongs to yet, which is the whole reason triage
exists, so the store cannot be the thing that answers it.

```json
{"id":"c7f2","col":"doing","kind":"feature","title":"...","body":"...",
 "cwd":"/Users/jhickner/working/mux","priority":0,
 "backend":null,"model":null,"effort":null,
 "session":"<backend session id>","worktree":".claude/worktrees/c7f2",
 "base":"b519936","cost_usd":0.42,
 "log":[{"ts":0,"who":"triage|worker|you","text":"..."}]}
```

`base` is the sha the worktree branched from, recorded at spawn. Without it the
diff you review and the range the audit reads mean "whatever master is now",
which is not stable while other workers are landing.

Done cards are archived past an age, so the store stays a working set.

## Triage

A one-shot silent fork — the `sidechannel.c` shape, `fork` plus `mux -C dir
"prompt"`, read stdout — so the board stays responsive and nothing new has to
be threaded. It returns JSON: `kind`, `title`, `spec`, `cwd`, `priority`,
`confidence`, and `question`.

Kinds fall in two classes. `todo`, `data` and `reference` are wiki kinds: the
same turn hands them to the `w` skill and the card lands in `done` without ever
costing a worker. `feature`, `bug` and `chore` are work kinds and go to
`backlog`.

The third outcome is the important one. A cheap classifier told only to pick a
kind will always pick one, so the prompt licenses "I don't know" explicitly:
low confidence, or a question, puts the card in `unclear` with the question
recorded. Answering appends your reply to the body and re-triages. Two failed
passes and it stops trying — the card waits for you to set kind and cwd by
hand, rather than burning tokens in a loop over text that structurally will not
parse.

`unclear` is narrowly about text that did not parse. A worker that errors or
stalls stays in `doing` with a mark against it, because the context you need to
fix that is the tab and its transcript, not a field on a card.

## Workers

A worker is a workspace tab. That is the whole trick: `workspace.c` already
runs turns on every tab at once, publishes each to `livelist`, colours the tab
strip by status, and calls back through `workspace_on_finish()` when a turn
ends. The board spawns tabs and reads that machinery; it does not add a second
kind of running thing.

Starting a card creates the worktree in C — `git worktree add
.claude/worktrees/<id> -b worktree-<id>` — rather than asking the agent to.
Deterministic, and it means the spawn can hand the session a cwd that already
exists. The spec is written to `CARD.md` in that worktree as well as sent as
the first turn: a long session compacts, restarts, and loses its first turn,
and the file does not.

The worker reports through the card rather than through prose the board has to
parse. `mux --card-log <id> <text>` and `mux --card-status <id> <state>` let it
say what it is doing and when it is ready, which is the useful half of treating
the tracker as external memory.

`workspace_on_finish()` moves a finished card to `review` with the last reply
logged. A turn that ends without the worker saying ready, or a worker with no
events for long enough, gets a mark and — past a timeout — is reaped back to
`backlog` with the reason logged, so a stalled agent cannot hold a slot
forever.

Each card records what it cost, from `backend_result`. That is what makes the
quota headroom below more than a guess.

## Profiles, quota, and delegation

`struct mux_spec` in `muxcfg.h` is already a worker profile — backend, model,
effort, and standing instructions — so profiles are keyed by kind and reuse the
shape:

```json
{"profiles": {
  "triage": {"backend":"claude","model":"haiku","effort":"low"},
  "bug":    {"backend":"claude","model":"opus","effort":"high"},
  "feature":{"backend":"claude","model":"opus"},
  "chore":  {"backend":"claude","model":"sonnet"},
  "audit":  {"backend":"claude","model":"opus","effort":"high"}
}}
```

First hit wins: explicit `backend`/`model`/`effort` on the card, then the
profile for its kind, then the session's own defaults. A profile's `prompt`
becomes the worker's `session_set_system_extra()`, which is where "work in a
worktree, arrive at a testable state" lives rather than being retyped into
every spawn.

`session.c` already polls `backend_rate_limit` through `quota_poll()`, so the
board reads a number that exists. Two rules gate a start: don't begin above a
usage ceiling, and don't begin work that will die mid-flight — the board sees
usage before and after every card, so it keeps a rolling per-profile average of
quota burned and requires headroom against it. A reset inside the next few
minutes holds the card instead of delegating, because waiting beats running it
on a worse backend.

When the head backend has no headroom, a delegation chain — `claude → codex →
grok` — hands the card to the first link that does, and logs why. Backends
reporting `available == 0` do not report limits at all, which makes them the
natural tail of the chain: the escape hatch when everything metered is spent.

## Audit and merge

The audit is a gate a card may skip, not a stage every card walks. `git diff
--stat` against the card's `base` decides: past a file or line threshold it
runs, under it the card goes straight on. You can always force one, and always
skip one, from the approve row — which says which it will do, so pressing it is
never a surprise.

An audit is a worker in the same worktree running the review skills already
installed, scoped to architecture, duplicated mechanisms, memory, and security.
Findings go in the log and the card returns to `doing`; clean passes go on.
Audit workers need skills, so they cannot run in safe mode.

`merging` is serialized, one card at a time: rebase onto master, build, test,
merge, remove the worktree. Any step failing sends the card back to `doing`
with the output logged and the worktree kept.

Findings accumulate per-cwd, and a refactor sweep reads them. Incremental work
duplicates mechanisms; the sweep looks for that and files ordinary cards into
`new`, so the board feeds itself. It counts cards rather than minutes — mux has
no daemon and the board only exists while it is open, so a timer would fire
when nothing was watching, while a counter trips exactly when you have been
using the thing.

## The view

Two screens, both `pick_run_live()`. Columns are group headings, cards are
rows, and the status column carries a spinner while a worker's turn is in
flight. There is no grid: mux uses no alternate screen anywhere, a rotated
board reads fine in a terminal, and a column layout is not worth a new
renderer until the list actually annoys someone.

```
▌ board · ~/working/mux · 2/3 workers · claude 71%

  unclear
    ? "fix the thing with the tabs"      which repo?

  backlog
    ● telegram menus lose the cancel row  bug · 3h
    ● worktree cleanup after approve      waiting on quota · 14:20

  doing
  → ⣾ kanban store and /card capture      tab 2 · 4m

  review
    ✓ sessions: say why a rename failed   ready · 20m
```

Enter always opens the detail view, one keystroke deeper, and the detail
view's first action opens the worker. Card content renders as dim unselectable
heading rows; the selectable rows are the actions, spelled out, so nothing has
to be memorised while the letters still work as shortcuts.

```
▌ c7f2 · feature · ~/working/mux

  spec
    Add a kanban store and /card capture. Cards land in a
    JSONL file at ~/.config/mux/board.jsonl, one per line.

  worker
    tab 2 · claude · opus 5 · working · 4m
    .claude/worktrees/c7f2 on worktree-c7f2

  log
    10:04  triage  feature · mux · priority 0
    10:31  worker  READY: build, then run /board

  → open the worker                              enter
    approve — 3 files, 47 lines, no audit            a
    approve and audit anyway                         A
    send feedback                                    f
    reject, back to backlog                          r
```

There is no REPL inside a card. Opening the worker calls `workspace_show()` and
puts you in its actual tab — full transcript, full prompt, type whatever you
want at it — and the left arrow brings you back. Feedback from the board is for
when you would rather not leave: a line, logged, sent with `workspace_send()`,
card back to `doing`.

Configuration lives on the board too, behind `c`, rather than being scattered
into settings: worker cap, usage ceiling and reset hold, audit thresholds,
profiles, the delegation chain, and the sweep cadence.

Board workers are marked wherever sessions are listed. `struct live_session`
carries the card id, so any window's session list can tell a board worker from
a session you started by hand, with `◆` alongside the existing `▸` for this
window's tabs and `⇄` for another window's.

## What this deliberately is not

Gas Town scales to twenty or thirty agents under a coordinating mayor. The cap
here is `WORKSPACE_MAX`, and the review gate is the point rather than an
obstacle to it: at thirty agents you stop reviewing and start rubber-stamping.
There is no coordinator agent because you are the coordinator. There is no
standing cast of specialist roles because profiles-per-kind and a conditional
audit get the same routing without a population to keep alive.

## Order of work

1. Store, `/card`, `mux --card`, the board list, the detail view.
2. The config screen, profiles, triage, `unclear`.
3. Workers: worktree, `CARD.md`, spawn, `on_finish` to `review`,
   approve/reject/feedback, the watchdog, `◆` in the session view.
4. Quota ceiling and delegation; headroom averaging once there is data.
5. Conditional audit, then the merge queue.
6. The refactor sweep, priorities per kind, archiving.
