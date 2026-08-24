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
new → unclear → backlog → active → review → [audit] → merge → done
        ↑__________|         ↑_______|________|_________|
```

Triage moves a card out of `new`. Work kinds land in `backlog` and wait for a
worker; wiki kinds are filed and go straight to `done`; anything the classifier
could not parse goes to `unclear` and waits for a sentence from you.

A worker takes a card from `backlog`, works in a worktree of its own, and puts
it in `review` when it has something testable. You approve, reject, or send
feedback. Approval optionally routes through an `audit` — an LLM pass over the
diff — and then through `merge`, which is serialized, because parallel
workers branched off the same base do not land cleanly on their own.

Every arrow back to `active` carries the reason with it, in the card's log, so
whoever picks the card up next reads why it bounced.

## Cards

One store, at `~/.config/mux/board.jsonl`, one card per line, written whole
under an exclusive lock — the same shape `reminders.c` uses. It is machine-wide
rather than per-repo: `cwd` is a field on the card, and the board filters by it,
defaulting to the directory you opened it from. A card thrown from the phone
does not know which repo it belongs to yet, which is the whole reason triage
exists, so the store cannot be the thing that answers it.

```json
{"id":"c7f2","col":"active","kind":"feature","title":"...","body":"...",
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
be threaded. It returns JSON: `kind`, `title`, `spec`, `cwd`, `confidence`,
and `question`. Priority is not among them: it follows from the kind, out of
the table in the config screen, so the classifier is not asked to invent a
number on top of everything else. A card can still be given one by hand.

`cwd` is not the directory the card was captured in. The `projects` setting
names the directory the repos sit in, `~/working` by default, and its
subdirectories are listed to the classifier with the card so a card thrown from
the phone, or from the wrong repo, lands on the project it names. Nothing there
matches, or the setting is empty, and the capture directory stands.

Kinds are configuration, not code. `board.json` holds the list: each has a
name, what the classifier is told it means, a priority, the prompt a worker
gets, the steps a card of that kind takes once a worker has had it --
`worktree`, `review`, `audit`, `merge` -- and the prompt approving one in
review sends. The triage prompt is written with a `{kinds}` mark where the list goes,
so editing the kinds edits what the classifier is told.

Every card goes to `backlog` and waits for a worker; nothing is filed without
one. What differs is what the worker is told and what happens after it stops.
`todo`, `data` and `reference` take no steps at all: the worker writes the note
into the wiki with the `w` skill and the card is done. `feature`, `bug` and
`chore` take all four, so they get a worktree, stop for a person, may be
audited, and land through the queue. A kind between the two -- landing without
a human stop, say -- is a matter of which steps it lists.

`prompt` is what a worker is told when it takes the card; `approval prompt` is
what it is told when you approve one in review. A kind that carries the second
is not finished by `a`: the worker is sent it and the card goes back to
`active`. The prompt is sent once -- the card's log is what says whether it has
been -- so the turn answering it does not stop in `review` a second time. It
goes on to whatever step follows, which for a kind that takes no others is
`done`, with what the worker did in the log. `buy.md` is that shape --
`steps: review`, a prompt that searches Amazon with the `web` skill and stops
on a shortlist, and `approval prompt: Order it, and say what was ordered.` --
so buying something is a file in `kinds/` rather than a branch in the board.

Both prompts take a `{id}` mark, replaced with the card's id. Two cards of the
same kind running at once share whatever their prompt names -- the `buy` prompt
drives a browser window through the `web` skill, and one window driven by two
workers is two workers fighting over a page -- so the mark is how a prompt names
a window, a file or a port that belongs to that card alone: `web --name
web-{id}`, checked with `web --endpoint`.

`plan` is the kind that asks for the work to be worked out rather than done.
It takes `steps: review` and no worktree, so the worker reads the repo and
answers with a plan, and what it answered becomes the card's spec: the plan is
what you read in review and what you edit before approving. Approving it files
a second card carrying the plan whole, under the title of the card that asked
for it. `next kind` on the kind file says what that card is -- `next kind:
feature` puts it straight in `backlog` at that kind's priority, and an empty
one sends it through `new` and triage like any other capture.

The third outcome is the important one. A cheap classifier told only to pick a
kind will always pick one, so the prompt licenses "I don't know" explicitly:
low confidence, or a question, puts the card in `unclear` with the question
recorded. Answering appends your reply to the body and re-triages. Two failed
passes and it stops trying — the card waits for you to set kind and cwd by
hand, rather than burning tokens in a loop over text that structurally will not
parse.

`unclear` is narrowly about text that did not parse. A worker that errors or
stalls stays in `active` with a mark against it, because the context you need to
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

board reads a number that exists. Two rules gate a start: don't begin above a
usage before and after every card, so it keeps a rolling per-profile average of
on a worse backend.

grok` — hands the card to the first link that does, and logs why. Backends
reporting `available == 0` do not report limits at all, which makes them the
natural tail of the chain: the escape hatch when everything metered is spent.

## Audit and merge

The audit is a gate a card may skip, not a stage every card walks. `git diff
--stat` against the card's `base` decides: past a file or line threshold it
runs, under it the card goes straight on. You can always force one, and always
skip one, from the approve row — which says which it will do, so pressing it is
never a surprise. A card sitting in a step is skipped with `k`, which stops
whatever is running for it and moves it on to whatever its kind takes next.

Which steps that works on is configuration, not code. Roles are the files in
`board/roles/`, one file to a role, and the file carries the whole definition:
`job` is the work it performs, `tier` how much model it is given, `step` the
stage of the flow it stands in, `skippable` whether that stage may be passed
by hand, and the body is its prompt. A field left out follows from the file
name — `audit.md` takes the `audit` job, and a job named after a step stands
in that step.

No list of roles exists in the board. `boardcfg_for_job("audit")` returns the
role whose file claims that job, so `audit.md` renamed to `reviewer.md` with
`job: audit` audits exactly as before, and a file dropped into `roles/` is a
role on the same terms as the ones that shipped. A job no file claims does not
run.

An audit is a worker in the same worktree running the review skills already
installed, scoped to architecture, duplicated mechanisms, memory, and security.
Findings go in the log and the card returns to `active`; clean passes go on.
Audit workers need skills, so they cannot run in safe mode.

`merge` is serialized, one card at a time: rebase onto master, build, test,
merge, remove the worktree. Any step failing sends the card back to `active`
with the output logged and the worktree kept.

Findings accumulate per-cwd, and a refactor sweep reads them. Incremental work
duplicates mechanisms; the sweep looks for that, so the board feeds itself. It
is a worker like any other: a card of its own in `active`, a tab `g` reaches,
and what it proposes waits on that card in `review`. Approving files the
proposals as ordinary cards in `new`, rejecting drops them, and editing the
card's spec first is how you drop one of several. `w` runs one on the spot for
the selected card's repo, whatever the count stands at. It counts cards rather
than minutes — mux has no daemon and the board only exists while it is open, so
a timer would fire when nothing was watching, while a counter trips exactly when
you have been using the thing.

## The view

Tab on an empty prompt opens the board and tab leaves it for the session list,
so the two views are one keystroke apart in either direction.

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

  active
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
    cancel starting, back to backlog                 x
```

There is no REPL inside a card. Opening the worker calls `workspace_show()` and
puts you in its actual tab — full transcript, full prompt, type whatever you
want at it — and the left arrow brings you back. Feedback from the board is for
when you would rather not leave: a line, logged, sent with `workspace_send()`,
card back to `active`.

Configuration lives on the board too, behind `c`, rather than being scattered

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

## The record

The card's log says what changed. `~/.config/mux/board-log/<id>.md` says why:
every stage the card went through, what that stage was asked, and what it
answered -- the triage prompt and its JSON, the audit's verdict, the merge
script and what git said to it, each worker's opening turn and its reply. `l`
on the board opens it.

It is markdown because the reader is a person with a question, and it is kept
apart from the card because it grows without bound and nothing but a person
ever reads it. A deleted card takes its transcript with it; an archived one
keeps it.

## Order of work

1. Store, `/card`, `mux --card`, the board list, the detail view.
2. The config screen, profiles, triage, `unclear`.
3. Workers: worktree, `CARD.md`, spawn, `on_finish` to `review`,
   approve/reject/feedback, the watchdog, `◆` in the session view.
5. Conditional audit, then the merge queue.
6. The refactor sweep, priorities per kind, archiving.
7. Kinds as configuration: the classes, what they mean, their prompts, and
   the steps each takes.
8. A transcript per card, and a worker for the cards the queue cannot land.
