# /mux command capabilities

I want to add some more capabilities to the /mux command.
- send a followup command to all sessions
- continue the conversation in a particular session, jumping into it in a new session


worktree /Users/jhickner/working/mux/.claude/worktrees/fngf

## Plan

This is the plan, in full. It supersedes the two bullets above where they differ: those were the original ask, and three rounds of review moved the design on. Read this, not them.

Turn `/mux` from a one-shot fan-out that prints a table into a live view: cells
you click to talk to one session, one input at the bottom that sends a followup
to all of them, and a way to take the selected cell into the sessions list as a
tab. The cells are children of the session that opened them, and `/sessions`
shows that.

Three phases. **Phase 0 is a refactor with no user-visible change** and exists
because the matrix needs two things mux has already built two or three times
each. Phase 1 is the view. Phase 2 is the parent link and the tree in
`/sessions`.

### The wheels that already exist

Before adding anything, here is what is already there and what is already
duplicated.

**Shared and healthy.** `replframe` renders a `Repl` into a cell grid
(`replframe.h`), and `replkeys` maps a `tty_event` to a `ReplEvent` and handles
paste (`replkeys.h`). Every input in mux sits on both. Neither needs changing.

**Duplicated three times.** The wrapper *around* those two — the sequence
set-width → ask-for-rows → render → scroll the caret into view → paint rows →
route keys with `^V` — is written out independently in `prompt.c`, `ask.c` and
`form.c`:

| step | prompt.c | ask.c | form.c |
| --- | --- | --- | --- |
| width | `:444` | `width_of()` `:29` | `repl_width()` `:171` |
| rows wanted | `:265` (+ dropdown) | `:45` | `:400` |
| `replframe_render` | `:270`, cached on `frame_ok`/`painted_cols` | `:47` | `:474`, cached on `st->framed` |
| caret scroll | n/a, the chrome grows | `scroll_to_caret()` `:52` | inline in `paint` `:523` |
| paint a row | `emit_input()` `:224`, custom cell walk | `replframe_paint_row` `:100` | `put_value_row` `:481` |
| keys + `^V` | `:690`, `:708` | `:150`, `:158` | `:884`, `:893` |

`ask.c` and `form.c` are near line-for-line the same for the single-field case.
`prompt.c` differs only in the paint step, where it rewrites the `>` glyph and
the synthetic caret. A matrix that rolls its own input makes this four.

**Written once, but in the wrong place.** The line → session pipeline lives as
inline control flow in `main.c`'s loop (`main.c:748-784`): bash line? mid-turn
command? `cmd_dispatch`? else `workspace_send`. `cmd_dispatch` is already
session-parameterized (`cmd.c:983`), so nothing about that sequence is
prompt-specific except the branches above it. A matrix that calls
`workspace_send` directly would be a second, subtly different answer to "what
happens when you submit a line" — one where `/model` silently does nothing.

**Deliberately left alone.** Each modal writes its own `tty_read` loop —
`ask.c:135`, `form.c:790`, `boardgrid.c:761`, `pick.c`. Their key semantics
differ enough (search, menus, fields, grid layout) that folding them together is
a larger refactor than this feature earns. The matrix makes a fifth. Phase 0
removes the *input* half of each, which is the part that is actually the same.

### Phase 0 — one input widget, one submit path

#### `src/replbox.{c,h}`

One struct owning a `Repl`, its `replframe`, its width, and its scroll window:

```c
struct replbox;

void  replbox_init(struct replbox *b, const ReplCommand *cmds, int n);
void  replbox_free(struct replbox *b);

void  replbox_width(struct replbox *b, int cols);
int   replbox_rows(struct replbox *b);              /* rows the text wants   */
void  replbox_room(struct replbox *b, int rows);    /* rows it may paint in  */
void  replbox_paint_row(struct replbox *b, int y, int gutter, int focused);
int   replbox_top(const struct replbox *b);         /* caret-following scroll */

int   replbox_key(struct replbox *b, const tty_event *ev); /* consumed?      */

const char *replbox_line(const struct replbox *b);
void        replbox_reset(struct replbox *b);
Repl       *replbox_repl(struct replbox *b);        /* for the odd caller    */
struct replframe *replbox_frame(struct replbox *b); /* for a custom painter  */
```

`replbox_key` handles `^V` via `replkeys_paste` and otherwise `replkeys_map` +
`repl_handle_input`, returning 0 for anything it did not consume so the caller
keeps Enter, Escape, Tab and its own bindings. That split is what makes one
widget serve three very different hosts.

**Convert `ask.c` and `form.c` in the same change.** This is not optional
polish — it is the whole point. An abstraction that only the new code uses is
the fourth way, not the one way. `ask.c` should end up as a modal shell around a
single `replbox` and lose `width_of`, `room_for`, `framed` and
`scroll_to_caret` outright; `form.c` should lose `framed`, `put_value_row`,
`repl_width` and the scroll arithmetic at `:523`.

**Leave `prompt.c` on its own painter, deliberately.** It is the main input
path and carries history, the completion dropdown, overlays, typeahead, queued
and external lines. `replbox_frame()` exists so it *can* move later without
giving up `emit_input`'s glyph rewriting, but moving it is not this plan.

**Acceptance test for phase 0: the diff deletes more input-plumbing lines than
it adds.** If it does not, the abstraction is wrong and should be abandoned
rather than shipped alongside what it failed to replace.

#### `cmd_submit()`

Lift the tail of `main.c`'s loop into `cmd.c`, beside the dispatcher it wraps:

```c
enum cmd_result cmd_submit(struct session *s, const char *line);
```

Mid-turn command → `cmd_dispatch_live`; command → `cmd_dispatch`; otherwise
`workspace_send(workspace_index_of(s), line, NULL)`. Resolving the tab from the
session rather than from `workspace_index()` is also a small correctness win:
one source of truth for "which tab is this line for".

`main.c` keeps the branches above it — bash lines, external/Telegram lines,
queued lines — because those are properties of the prompt, not of a session, and
calls `cmd_submit` for the rest. The matrix calls it once per target cell.

The payoff: `/model`, `/effort`, `/thinking`, `/permission` all work on the
selected cell for free, which is exactly what a matrix of differently-configured
agents wants. And the commands that open a picker are already safe inside a
modal — `can_pick()` refuses with "a list is already open" when
`chrome_modal_active()` (`cmd.c:213`).

### Phase 1 — the view

#### The decision that makes this cheap: a cell is a workspace tab

Each matrix row becomes a real `struct session` opened as a workspace tab,
instead of `fanout.c`'s `struct worker` around a raw `Backend` on a `pthread`:

| want | mechanism | evidence |
| --- | --- | --- |
| a clickable surface | a chrome modal, which owns `tty_read` and builds its own row map | `form.c:853`, `boardgrid.c:876` |
| talk to one cell | `cmd_submit()` → `workspace_send`, which queues when that cell is mid-turn | `workspace.c:525` |
| followup to all | the same call in a loop | |
| cell as a tab in the sessions list | it already is one — `^s` lists `ROW_TAB` off `workspace_at()` | `sessionswitch.c:380` |
| turns run under the modal | `workspace_pump_quiet()` from the tick; `pump()` holds the paint | `workspace.c:412,464` |
| a cell paints nothing over the modal | a session that is not the live one draws nothing | `session.c:235` |
| cell content | `session_set_observer()` feeds the cell's own log | `session.c:232` |

Why a modal and not a clickable scrollback item: the viewport turns mouse
reporting on for the whole session (`viewport.c:1416`) and `TK_MOUSE_DOWN`
carries row and column (`tty.c:454`), but `viewport.h` exposes no hit-testing,
and the REPL loop drops the event into the line editor (`prompt.c:818`).

Two consequences: **the observer runs on the main thread** — events are queued
while a turn is in flight (`session.c:363`) and replayed by `drain_events()`
inside `session_turn_pump()` (`session.c:1775`), so the cell log needs no lock,
and `board_lock`, `atomic_int done` and `fan_work()` all go. And **the
`ephemeral` problem disappears** — it only existed because `fan_work()` set
`o.ephemeral = 1` (`fanout.c:583`); `session_new()` never does, so cells are
resumable and replayable from the first turn.

#### Layout

```
┌────────────┬──────────────────────────────────────────┐
│ matrix     │ <the last line sent to all>              │
├────────────┼──────────────────────────────────────────┤
│ 1 claude   │ › <standing prompt>                      │   selected cell:
│   sonnet   │ the rows of its log, tail-anchored       │   takes the spare rows
├────────────┼──────────────────────────────────────────┤
│ 2 codex    │ …                                        │
└────────────┴──────────────────────────────────────────┘
 all ›  the input                                            one replbox
 ↑↓ pick a cell · enter go · esc all · ^c leave
```

One input with a target: its prefix says who it talks to — `all ›` by default,
`2 codex ›` when a cell is selected. Enter sends and clears.

#### The key map takes nothing that is already bound

Every control code is spoken for. The line editor consumes
`^A ^C ^E ^K ^R ^U ^W ^Y ^_` (`vendor/repl.h:1087`); the prompt binds
`^B ^C ^D ^F ^G ^L ^N ^O ^T ^V` (`prompt.c:694`); `^H ^I ^J ^M` are
backspace/tab/newline/enter; `^Q ^S ^Z` belong to the terminal. That leaves `^P`
and `^X`, and either still costs the `p`/`x` muscle memory from the sessions
list (`sessionswitch.c:31`).

So the view binds **no new keys**. It reuses the disambiguation idiom the prompt
already uses, where a key means one thing with text in the buffer and another
when it is empty (`prompt.c:783` for Enter, `:820` for left, `:838` for up):

| key | with text | with the input empty |
| --- | --- | --- |
| `↑` / `↓` | move the cursor | move the cell selection |
| `enter` | send to the target | go to the selected cell as a tab, and leave |
| `esc` | — | first press targets all again; second leaves |
| `^c` | clear the line (the repl's own binding) | leave |
| click a cell | select it | select it |
| click the selected cell | go to it as a tab, and leave | same |
| wheel / `page up`·`page down` | scroll that cell | same |

Enter-on-empty carries "go to this cell", so nothing is invented and nothing is
shadowed — sending an empty followup was never meaningful. Second-click-activates
matches the board grid (`boardgrid.c:876`).

#### Steps

1. **Rename `src/fanout.{c,h}` → `src/matrix.{c,h}`.** `SRC` is a wildcard
   (`Makefile:19`), so no build change; update the include at `cmd.c:15`.
   Keep `struct entry`, `log_add`, `entry_painted`, `rowbuf`, `rule`,
   `table_row`, `worker_block`, `standing_row`, `board_stacked`,
   `label_width` — that renderer, a conversation painted into a fixed-width box
   and cached per width, is the reason to grow this file rather than start one.
   Delete `fan_work`, `fan_event`, `board_lock`, `aborted`, `show_thinking`, the
   `pthread_create`/join blocks and the 15ms poll loop.

2. **`struct cell` replaces `struct worker`:** the `mux_spec`, a
   `struct session *` as identity, the session id kept for when the tab closes,
   an `adopted` flag, and the log and row cache verbatim. Re-derive the tab
   index with `workspace_index_of()` on use — **indices shift whenever a tab
   closes**, which `pump()` already walks around at `workspace.c:432`. The left
   column now reads `session_model()`, `session_effort()` and
   `workspace_status()`, plus the row number, which nothing prints today and the
   input prefix needs.

3. **`cell_event()`** is today's `fan_event` with the locking removed,
   registered with `session_set_observer(s, cell_event, cell)`. Add one entry
   kind, `CELL_SENT`, painted with `ui_wrapped(text, 0, UI_ECHO)`: a cell taking
   many turns has to show what it was asked each time.

4. **Spawning and retiring.** Add
   `workspace_spawn_ex(backend, model, effort, cwd, id, system)` with
   `workspace_spawn()` delegating with `NULL` — `session_set_system_extra()` has
   to land before `session_start()` builds the backend (`session.c:836`), and
   the row's standing prompt is that system text. Six call sites are untouched.
   `session_set_naming(s, 0)` then `title_set(id, label)`: every session
   otherwise forks a haiku call to name itself (`session.c:552`), 3 to 12 per
   run. Build `matrix · <backend> · <prompt>` and clip with `ui_fit_bytes()` —
   `title_set()` runs `tidy()`, which **rejects** anything over 80 bytes rather
   than truncating, and a byte-wise clip splits UTF-8. `MUX_MAX` is 12 and
   `WORKSPACE_MAX` is 12 and the opener holds one, so refuse rows past the free
   slots and say which were dropped. Leaving retires the idle, unadopted cells
   with `workspace_close()`, keeping their ids in a file-static snapshot; cells
   mid-turn and cells that were adopted stay as ordinary tabs, so no in-flight
   work is destroyed and nothing kept on purpose disappears.

5. **The modal**, on `form_run()` for the input and `boardgrid_run()` for the
   loop: `chrome_full(1)`, `chrome_modal(paint, &view)`, loop on
   `tty_read(&ev, PICK_POLL_MS)`, `chrome_paint()` after any change, honour
   `chrome_modal_interrupted()`. Every tick and timeout calls
   `workspace_pump_quiet()` — that is what advances the turns, drains the event
   queues and fills the cell logs. The input is one `replbox`. Build the
   row→cell map during `paint` into a `short hit[HIT_MAX]` and read it as
   `ev.row - 1 - viewport_chrome_top() - chrome_gap()`, exactly as `form.c:853`
   does; the table already paints each cell as a contiguous run of full-width
   rows. The selected cell takes the rows the others do not need, minimum half
   the body; the rest split what is left, minimum 3, tail-anchored — without
   this, "talk to that session" happens in a 3-row window.

6. **`do_mux`:** `/mux <prompt>` opens the cells, sends to all, runs the view;
   bare `/mux` reopens on the snapshot, falling back to today's listing;
   `config` and `make` unchanged. **Keep a headless path** — `form_run` refuses
   without `frontend_has_keyboard() && tty_is_raw()` (`form.c:788`) and `/mux`
   must keep working over Telegram and from a pipe. Same engine, different
   shell: spawn, send, pump until every cell is idle, paint the board once as a
   viewport item as it works today, close the cells.

### Phase 2 — parentage, and the tree in `/sessions`

A session gets one parent, fixed when it is created, so the graph is a forest,
not a general DAG. Worth holding onto: one field admits no cycles by
construction, and every question the UI asks is one lookup.

#### Where the link lives

Model it on `title.c`, which solves the same problem — a fact about a session id
that must outlive the process, be visible to other windows, and survive a
resume:

```c
/* src/parent.c */
int parent_set(const char *child_id, const char *parent_id);
int parent_of(const char *child_id, char *out, size_t size);
```

An append-only `parent` file beside `titles` under `~/.config/mux`, read by a
linear scan with the last row winning (`title.c:21` and `:65` are the shape).
That carries across `/restart`, across windows, and into a session resumed
months later, with no new field in the tabs file (`main.c:83`) and no new
command-line flag.

`session_set_parent(s, parent_id)` holds the id until the child's own id is
known, and `set_id()` (`session.c:982`) — the one choke point where an id first
appears — writes the row. In practice it is known immediately: `restart()` reads
`b->session_id(b)` right after `start()` (`session.c:1027`), with `name_poll()`
as the fallback for backends that report it later.

Publish it too: `livelist_publish()` already looks the title up by id and writes
it into the live record (`livelist.c:216`), so add a `parent` field beside it
and a `char parent[128]` to `struct live_session`. Other windows' sessions then
carry their parentage without every reader scanning the file.

**Trap:** the opening session may have no id — a session that has taken no turn
has an empty `s->id`, and `/mux` as the first thing typed in a fresh tab is
exactly that. The children must open anyway and record no parent. Do not block
the matrix on it and do not invent a placeholder id.

#### What `/sessions` shows

`/sessions` is already two levels — rows grouped by directory under `ROW_HEAD`
headings (`sessionswitch.c:223`) — and every label already starts with a marker:
`▸` for the current tab, `·` for the others (`:84`), `→`/`⇄` for another
window's (`:160`). Nesting is a prefix and an ordering change, not a new
rendering mode:

```
~/working/mux
  ▸ fix the parser (here)          claude opus
    └ matrix · claude              claude sonnet
    └ matrix · codex               codex gpt-5
  · unrelated session              claude opus
```

- **Ordering.** In `group_rows`, after emitting a row, emit its children
  depth-first before moving on. Cap the depth at 4 and carry a visited set: the
  parent file is hand-editable and a cycle there must not hang the list.
- **Indent.** `depth * 2` spaces and `└` in place of the row's own marker.
  `align_width()` measures `ui_cells(label)` (`pick.c:84`), so the detail column
  keeps lining up on its own.
- **A child in another directory.** Matrix cells inherit the opener's cwd, so
  normally they share a group. When they do not, leave the child at top level in
  its own group rather than nesting it under a heading that would then be lying
  about its rows, and name the parent in the child's detail. **This is the
  judgment call in phase 2** — "parentage beats directory grouping" is
  defensible and changes `group_rows` more deeply.
- **Other windows' rows nest too**, on the same rule, since the parent id rides
  along in the live record.
- **A closed parent orphans no one.** Nest only when the parent is present in
  the list; otherwise the child sits at top level. That falls out for free.
- `/session` grows one row — `parent   <title or id>` — in the block at
  `session.c:2053`.

#### Keep the link general, use it narrowly

`session_set_parent` must not know what a matrix is. Two other places already
create a session from a session: `^b`'s new tab (`main.c:252`) and the board's
worker tabs (`boardwork.c:734`). **Do neither in this plan** — `^b` in
particular would start nesting rows in everyone's `/sessions` for a relationship
nobody asked to see. Land the mechanism, use it for `/mux`, leave the rest a
separate decision.

### Housekeeping — sweep the orphaned board-cards directory

Unrelated to the view. It rides along because this is the tree that is open.

`~/.config/mux/board-cards/<id>.md` held the verbatim CARD.md copies that
`boardfile.c` kept at the end of every worker turn. That module is gone and
`boardfile_drop` went with it, so the directory is orphaned: nothing reads it,
nothing writes it, nothing removes it. Fifty files on this machine.

Do not write a sweeper for it. `drop_stale(leaf)` (`boardcfg.c:142`) already
does this exact job — list `<config>/<leaf>/*.md`, unlink each, `rmdir` the
directory — and `read_all()` already calls it for four leaves
(`boardcfg.c:434`), for the same reason: files the binary used to own and no
longer does. Add a fifth call:

    drop_stale("board-cards");

`board_md_path()` built those paths as `<config>/board-cards/<id>.md`
(`board.c:106`), which is the shape `drop_stale` reconstructs from
`mdcfg_list`, so the names line up with no change to either side.

Traps:

- `STALE_MAX` is 64 and `mdcfg_list` stops there, so a directory holding more
  than 64 files takes more than one board open to drain and the `rmdir` fails
  quietly until the last pass. It is self-healing; do not read the first
  failed `rmdir` as a bug.
- These files are the last surviving copy of the plan for any card that was
  closed without merging before CARD.md moved into git. Sweeping is deliberate
  and irreversible. Anything worth keeping should be copied aside before this
  ships.

### Traps

- **Tab indices move.** Any `workspace_close` shifts every later index down.
  Hold the `struct session *`.
- **`^s` from inside the matrix.** The sessions list is itself a modal; do not
  open it re-entrantly. Enter-on-empty leaves the matrix first, then shows the
  tab. `can_pick()` already refuses pickers under a modal (`cmd.c:213`), which
  is what makes `cmd_submit` safe to call from the view.
- **Two paths show the user's line in a cell**: `CELL_SENT` live, and the
  transcript on replay through `sessionload_replay`. Both must paint `UI_ECHO`
  or a resumed cell will look different from a live one.
- **`/restart` restores cell tabs as ordinary tabs.** `restore_tabs()`
  (`main.c:83`) writes every tab out and brings it back, so cells return as
  plain sessions with no view around them — acceptable, and with phase 2 they
  still know their parent. But a reopened `/mux` must not double-spawn a row
  whose id is already a tab; `workspace_find_id()` is the check.
- **A cell that fails to start.** `workspace_spawn` returns -1 and the row has
  no session; it still needs a cell in the table painting the failure, or the
  matrix silently shrinks.
- **The opening session is a tab too.** Never adopt, retire or send to it as a
  cell.
- **Interrupting one cell.** Nothing in the key map stops a cell mid-turn.
  Either `session_interrupt()` gets a place or the hint says plainly that it
  cannot be done here. Do not leave it ambiguous.
- **Width.** `board_render` falls back to a stacked layout under `FAN_BODY_MIN`
  (24 body cells). In a modal that fallback must keep the input and the hit map
  working, or the view refuses narrow terminals the way `boardgrid_run` returns
  `GRID_NARROW`.
- **Matrix runs now show in `/resume`.** They always would have once persisted;
  the `title_set` in step 4 is what keeps them legible.

### Verification

- `make` — the warning set is strict (`-Wall -Wextra -Wmissing-prototypes`).
- `make check` — no harness links `matrix.o`, `cmd.o` or `workspace.o`, so this
  guards only collateral damage. Three pieces here are pure and worth harnesses
  in the style of `tools/boardgridtest.c`: `replbox`'s scroll window (a caret
  past the bottom of the room scrolls it into view and no further), the row→cell
  hit map, and phase 2's ordering pass — feed it a parent map with a cycle and a
  missing parent and check it terminates and orphans nothing.
- **Phase 0 has to be checked by reading the diff**, not by running anything:
  `ask.c` and `form.c` must come out shorter, and `width_of`, `room_for`,
  `framed`, `scroll_to_caret`, `put_value_row` and `repl_width` must be gone.
- Manual, in a pty: set `MUX_LIVE_DIR` and drop `TMUX_PANE` so the test instance
  does not haunt the `^s` list, and do **not** override `HOME` — auth is in the
  keychain and a fake `HOME` pops a browser login. Walk: `/mux what is 2+2` →
  three cells answer → click cell 2 → type → only that cell moves → `/model` in
  cell 2 → the picker refuses politely rather than opening under the modal →
  `esc` → type → all three move → `enter` on an empty input → that cell is the
  front tab → `^s` shows it indented under the session that opened it → back to
  `/mux` → cell 2 is still there, 1 and 3 came back from their saved ids.
- Mouse: check inside and outside tmux. `sync_frames()` disables synchronized
  updates under tmux (`viewport.c:92`) and the modal repaints on every pump.

## Notes

- board: started
- board: started
- plan: `plans/mux-followup-and-open.md`

**The finding that shapes everything:** `fanout.c` opens every matrix row with `o.ephemeral = 1` — `--no-session-persistence` for claude, `no_session` for grok/pi, an unmaterialized thread for codex. Nothing from a `/mux` run is resumable today, and no transcript is written. Both requested capabilities are the same fix: drop `ephemeral`, capture each row's `session_id()` after its turn, and reopen against it.

**The plan:**

- `/mux followup <prompt>` — reopens each saved row with `resume_session` and runs the same board. Resume-by-id rather than holding 12 CLI children alive between commands.
- `/mux open [row]` — `workspace_spawn()` a tab adopting that row's id, then `sessionload_into()` to replay it, mirroring `yank()` in `sessionswitch.c`. Needs a small `workspace_spawn_ex()` so the row's standing prompt survives the jump (`system_extra` must be set before `session_start()`).
- `fanout_run()` splits into a shared `fan_go()` plus a file-static snapshot of the last run (specs, ids, and the cwd it ran in).

**Traps called out:** the followup must resume in the *stored* cwd, not the session's current one, or claude/codex won't find the transcript; opening a row must release its id from the snapshot so a later followup doesn't put two writers on one session; persisting rows means they now clutter `/resume`, mitigated by `title_set()` — which rejects rather than truncates labels over 80 bytes; `/restart` drops the snapshot.

Naming choices I made: no `more` alias (it would swallow `/mux more tests please`), and `open` counts as a subcommand only when followed by nothing or digits.
- plan: `plans/mux-matrix-view.md` (renamed — the old `mux-followup-and-open.md` is gone, since subcommands aren't the deliverable any more)

**What the research changed.** A scrollback item can't be clicked: mouse reporting is on globally and `TK_MOUSE_DOWN` carries row/col, but `viewport.h` has no hit-testing and the REPL loop drops the event into the line editor. Every clickable surface in mux — `form.c`, `boardgrid.c`, `pick.c` — is a chrome modal that owns `tty_read` and builds its own row map. So the matrix becomes a `chrome_full(1)` modal, like the board grid.

**And that unlocks the real simplification:** once it's a modal, each cell should be a workspace tab, not a fanout `struct worker` on a raw pthread. `workspace.c:412` already reads `hold = hold || chrome_modal_active()` — the workspace is built to drive sessions while a modal holds the screen. Then: talking to a cell is `workspace_send` (which queues if it's busy), the followup-to-all is that in a loop, and "access its session as a tab in the sessions list" is free — `^s` lists tabs straight off `workspace_at()`, so the hotkey is a jump, not a hand-off. Two bonuses: the observer that feeds each cell's log runs on the main thread (events are queued and drained in `session_turn_pump`), so `board_lock` and the atomics all go; and the `ephemeral` blocker from the last draft disappears, because `session_new` never sets it.

The plan keeps fanout's cell renderer intact — it's the one thing that already paints a conversation into a fixed-width box — and deletes the thread pool underneath it.

**One thing I want you to rule on before implementation:** I specced a *single* input at the bottom whose target switches (`all ›` vs `2 codex ›`) when you click a cell, rather than a per-cell editor plus a separate master. Your wording supports both readings; I picked one line because two live line editors on one screen is a lot of chrome for one prompt at a time. Also flagged: `^g` for "go to this cell as a tab" shadows `^g`-opens-$EDITOR inside the modal — say the word and it becomes `^o`.
- plan: `plans/mux-matrix-view.md` — same file, rewritten. Now in two phases: the view, then parentage.

**On not shadowing hotkeys — the receipts.** Every control code is already taken: the line editor eats `^A ^C ^E ^K ^R ^U ^W ^Y ^_` (`vendor/repl.h:1087`), the REPL prompt binds `^B ^C ^D ^F ^G ^L ^N ^O ^T ^V` (`prompt.c:694`), `^H ^I ^J ^M` are backspace/tab/newline/enter, and `^Q ^S ^Z` belong to the terminal. That leaves `^P` and `^X`, and both still cost the `p`/`x` muscle memory from the sessions list.

So the view binds **nothing new**. It reuses the idiom the prompt already uses — a key means one thing with text in the buffer, another when it's empty (`prompt.c:783`, `:820`, `:838`): arrows move the cursor or the cell selection; **Enter sends, or, on an empty input, goes to the selected cell as a tab**. Sending an empty followup was never meaningful, so that action was free. Clicking the already-selected cell does the same thing, matching the board grid.

**On the DAG.** One parent, fixed at creation, so it's a forest — no cycles by construction, and every UI question is one lookup. The link belongs in an append-only `parent` file beside `titles` in `~/.config/mux`, modeled on `title.c`, which already solves exactly this problem: a fact about a session id that must outlive the process, be visible to other windows, and survive a resume. That gets parentage across `/restart` with no new tabs-file field and no new CLI flag, and `livelist_publish` can carry it to other windows the same way it carries the title.

For `/sessions`: it's already two-level (cwd headings), and every row's label already starts with a marker glyph — so nesting is a prefix plus an ordering pass, not a new rendering mode. I've written the specifics and flagged the one genuine judgment call: **what to do with a child whose cwd differs from its parent's.** I proposed leaving it at top level in its own directory group rather than nesting it under a heading that would then misdescribe its rows — but "parentage beats directory grouping" is defensible and changes `group_rows` more deeply. That one's yours.

Two traps worth surfacing now: a session that has taken no turn has **no id**, so `/mux` as the first thing typed in a fresh tab produces children with nothing to hang under — they must open anyway and record no parent. And I deliberately scoped `^b`'s new tab and the board's worker tabs *out* — the mechanism should be general, but wiring `^b` to it would start nesting rows in everyone's `/sessions` for a relationship nobody asked to see.
- : `plans/mux-matrix-view.md` — same file, now with a **Phase 0** in front that's pure refactor, no user-visible change.

**You were right, and here's the specific duplication.** `replframe` (render a Repl into a cell grid) and `replkeys` (tty event → repl event, paste) are shared and healthy. What's written out three separate times is the wrapper around them — set width → ask for rows → render → scroll the caret into view → paint rows → route keys with `^V`. I put the line-by-line comparison in the plan: `prompt.c:444/265/270/224/690`, `ask.c:29/45/47/52/100/150`, `form.c:171/400/474/523/481/884`. `ask.c` and `form.c` are near line-for-line identical for the single-field case; `prompt.c` differs only in the paint step, where it rewrites the `>` glyph. A matrix rolling its own makes four.

So: `src/replbox.{c,h}` owns Repl + replframe + width + scroll window + key routing, with `replbox_key` returning 0 for keys it didn't consume so each host keeps Enter/Escape/its own bindings. **`ask.c` and `form.c` get converted in the same change** — that's the whole point, and I wrote the acceptance test as *the diff must delete more input-plumbing lines than it adds*. If it doesn't, the abstraction is wrong and should be dropped rather than shipped next to what it failed to replace. `prompt.c` stays on its own painter deliberately (history, completion dropdown, overlays, typeahead); `replbox_frame()` leaves the door open.

**A second wheel I found while looking.** The line → session pipeline exists once, but as inline control flow in `main.c:748-784`. `cmd_dispatch` is already session-parameterized, so I lifted the tail into `cmd_submit(session, line)`. The matrix calling `workspace_send` directly would have been a second answer to "what happens when you submit a line" — one where `/model` silently does nothing. Routing through `cmd_submit` instead means `/model`, `/effort`, `/permission` all work on the selected cell for free, and it's already safe: `can_pick()` refuses with "a list is already open" when `chrome_modal_active()` (`cmd.c:213`).

**One wheel I deliberately left spinning:** every modal writes its own `tty_read` loop (`ask.c`, `form.c`, `boardgrid.c`, `pick.c`), and the matrix makes a fifth. Their key semantics genuinely differ — search, menus, fields, grid layout — so unifying them is a bigger refactor than this feature earns. Phase 0 removes the input half of each, which is the part that actually is the same. Flagged rather than hidden.
- : Yes — written to `plans/mux-matrix-view.md` (398 lines), including everything from this round:

- **The wheels that already exist** — the inventory with file:line receipts for the three duplicated input wrappers, plus the two wheels I'm deliberately leaving alone.
- **Phase 0** — `replbox` (with the "convert `ask.c` and `form.c` in the same change" requirement and the diff-must-shrink acceptance test) and `cmd_submit()`.
- **Phase 1** — the view, cells-as-tabs, and the key map that binds nothing new.
- **Phase 2** — the parent link in a `parent` file beside `titles`, and the `/sessions` tree.
- Traps and verification, including the phase-0 check that's a diff read rather than a test run.

It's the same file as the last two revisions — only `plans/mux-matrix-view.md` exists; the earlier `mux-followup-and-open.md` was removed when the subject changed.

One thing still open from my side: the phase-2 judgment call on what to do with a child whose cwd differs from its parent's. I proposed leaving it at top level in its own directory group; the alternative — parentage beats directory grouping — changes `group_rows` more deeply.
- : No. Three facts:

- **`plans/` is untracked** — `git status` shows `?? plans/`. It's not ignored, just never committed, so it exists only in this worktree's working tree. Anyone on another branch, worktree, or machine sees nothing.
- **CARD.md can't hold a pointer.** It's generated from the board card by `card_write()` (`boardwork.c:315`) — title, body, worktree, and a `## Notes` section rendered from the card's log — and `boardwork.c:295` adds it to `.git/info/exclude`. Anything typed into it directly gets clobbered the next time the card is written out.
- **The repo's own convention was to commit it.** History shows `plans/triggered-actions.md` tracked on a branch, amended as the work went (`9151e10 plan: mark the traps that outlived the change`), and deleted when it landed (`366fcd2 remove plans`).

There's a sharper version of the problem: the card body still says the original two bullets — "send a followup command to all sessions", "continue the conversation in a particular session". That's no longer what the plan builds. An implementer who reads the card and never finds the plan builds the wrong thing.

Two things would fix it, and they're complementary:

1. **Commit `plans/mux-matrix-view.md` on `worktree-fngf`** — matches the prior convention, and puts the plan in front of anyone who checks out the branch.
2. **Add a note to the board card** pointing at `plans/mux-matrix-view.md` — the card log is the durable side of CARD.md, so the pointer survives regeneration and is the first thing whoever picks the card up reads.

I'm in plan mode, so I haven't done either — both are commits/writes outside the plan file. Say the word and I'll do them, or tell me which one you want.
- : First, a correction that changes the answer: **CARD.md is already a working write-back channel.** I said it gets clobbered — wrong. `boardfile.h` states the contract outright: *"The board writes it once when the worktree is made and the worker owns it from then on, so the board only ever takes a copy of it verbatim and puts that copy back."* `boardfile_keep()` runs at the end of every worker turn (`boardwork.c:879`) and copies the worktree's CARD.md to `~/.config/mux/board-cards/<id>.md`; `write_card_file()` restores that copy into any later worktree, generating from card fields only as a fallback (`boardwork.c:335`). This session is card `fngf` with `"done":["plan"]`, so my turn ends in a capture. Your preferred option B exists in the plumbing today — nothing points the planner at it.

**The real gap is worse than the one you asked about.** `implement.md` opens with *"The plan below was approved."* What's actually below is `card_write(out, c)` — the card in full, notes and all (`boardwork.c:441-470`). Nothing puts a plan there: `needs:` only sequences actions (`boardflow.c:20`), and `boardstep_fill` knows just `{branch} {worktree} {repo} {onto} {base}` (`boardstep.c:29`). The implementer is promised a plan it is never handed, and then falls back to the card body — which, for this card, still says the original two bullets the plan has long since outgrown.

**Recommendation — both halves, they're complementary:**

1. **Planner writes a `## Plan` block into CARD.md**: the path, a one-line synopsis, and "this supersedes the card body where they differ." It rides in `board-cards/<id>.md`, survives the worktree being dropped, and gets inlined verbatim into the implementer's first turn. That's B, and it needs no code.
2. **Planner commits the plan file.** `implement.md` already says "uncommitted work does not exist to anything downstream" — an untracked `plans/` file is exactly that. That's A's durable half: the card names the path, git carries the content. It also matches the repo's own history (`plans/triggered-actions.md` was committed, amended, then deleted on landing).
3. **Both prompts need editing**, because `plan.md` currently forbids exactly these two acts ("The one file you write is the plan file itself", "do not make any commit"), and `implement.md`'s first line should say what's true: read the file the card's `## Plan` names; it's the spec; it supersedes the card body.

One cost to know: action prompts are **compiled in**. `read_actions()` reads only `board_defaults` (`boardcfg.c:243`), and `boardcfg.c:435` actively deletes stale action files from the config dir — which is why `~/.config/mux/board/` has only `backends/` and `settings.md`. So this is a source edit to `board/actions/*.md` plus a rebuild, not a config tweak.

**If you want it robust rather than convention-based**, the code version is small: a `plan:` frontmatter key or a `{plan}` fill that inlines a named file into the action prompt, so the hand-off doesn't depend on the planner remembering. `first_turn()` is the one place to do it.

I can't do any of this from here — `plan.md` forbids the edits and the commit, which is the recursion in the problem. Want this written up as its own plan file? It's a different subject from the matrix view, so it'd be a new slug rather than a rewrite of `mux-matrix-view.md`.
- : Done. The plan now lives in `CARD.md` as a `## Plan` section between the card body and `## Notes`; `plans/` is gone, and the worktree is clean (CARD.md is git-excluded by design, `boardwork.c:295`).

The section opens by saying it supersedes the two bullets above, since those are the original ask and the design moved on. Headings are demoted so `## Plan` and `## Notes` remain the card's only two top-level sections.

**This survives on its own.** `boardfile_keep()` runs at the end of this turn (`boardwork.c:879`) and copies CARD.md to `~/.config/mux/board-cards/fngf.md`; `write_card_file()` restores that copy verbatim into any future worktree for this card, generating from card fields only when no copy exists. So the plan outlives this worktree.

**But one string blocks the hand-off.** I re-checked `first_turn()`: the prompt it builds inlines `card_write(out, c)` — the card from the *store* (title, body, worktree, log), not the contents of CARD.md — and then says:

> "Do not open it to start: it holds nothing you have not been given."

That was true when CARD.md was just a rendering of the card. It's false now, and it's the exact sentence that will make the implementer skip the plan. Two edits make your chosen approach work:

- `boardwork.c:464-467` — that sentence must instead tell the worker to read CARD.md, because the card in the prompt is no longer the whole of it.
- `board/actions/implement.md` — "The plan below was approved" should say the plan is the `## Plan` section of CARD.md, and that it supersedes the card body.

Both are compiled in (`read_actions` reads only `board_defaults`; `boardcfg.c:435` deletes stale config copies), so they need a source edit and a rebuild. And `board/actions/plan.md` still says "The one file you write is the plan file itself" and names `plans/<slug>.md` — that wants updating to the new convention too, or the next planner will recreate `plans/`.

I haven't touched any of those three — they're mux source changes, outside what plan mode permits me here. Want them as the next card?
- : Good change on balance — better on the three things that matter, and it deletes code. But two conditions, or it's a net regression.

**What genuinely improves.** Today there are two copies: the worktree's CARD.md and `~/.config/mux/board-cards/<id>.md`, reconciled by `boardfile_keep()` at the end of every worker turn. Git makes the branch the single record, and `boardfile.c` (~70 lines), both keep calls, and `ignore_card_file` all delete. You also get history — `boardfile_keep` is last-write-wins, whereas this plan's three revision rounds would have been three diffable commits. And it travels: the kept copy lives in one machine's config dir, a branch goes wherever the repo goes. As a bonus, implement.md's own rule ("uncommitted work does not exist to anything downstream") finally holds for the plan.

**Condition 1 — strip on the branch, before the merge, not after.** `merge.md` lands with `git merge --ff-only {branch}`. A fast-forward means master gets exactly the branch's commits, so if CARD.md is tracked at the tip, **master ends up with CARD.md at its root, plan and all.** The strip has to be a `git rm CARD.md` commit as the last step on the branch. That also disposes of a problem you'd otherwise hit fast: every card carries `/CARD.md`, so two cards merging to master would conflict on that one path every time. Stripped before the merge, they never meet. And it stays recoverable — after an ff-merge master's history holds both the add and the remove, so `git show <sha>:CARD.md` gets the plan back.

**Condition 2 — nothing may drop the worktree while CARD.md is dirty.** The `boardfile_keep(c)` call in `drop_worktree` (`boardwork.c:1131`) is today's safety net. Git has no equivalent, and `tidy_step` runs `git worktree remove --force` followed by `git branch -D` (`:227`) — force plus `-D` means an uncommitted or unmerged plan vanishes silently. `drop_worktree` has to refuse, or auto-commit, when the tree is dirty. This is the one place where "just use git" is strictly weaker unless handled.

**One regression to accept knowingly.** A card closed *without* merging gets `branch -D`, and the plan is gone; today the kept copy survives until the card is deleted. I'd accept that — an abandoned card's plan is usually worth abandoning — but make close-without-merge say what it's about to destroy.

**A gotcha you'd hit in the first five minutes.** `ignore_card_file()` (`boardwork.c:265`) appends `CARD.md` to `<git-common-dir>/info/exclude` — the *shared* git dir, so it's repo-wide and permanent. It's already in this repo; I tested it just now: `git add CARD.md` → "ignored by one of your .gitignore files", needs `-f`. That function has to go, and existing repos need the stale line removed, or every planner's `git add` silently no-ops.

**What this doesn't fix.** Delivery. The implementer's prompt still inlines `card_write(out, c)` — the card from the store — and still says *"Do not open it to start: it holds nothing you have not been given."* Git makes CARD.md durable, reviewable and shareable; it does not put it in front of the implementer. Those two strings (`boardwork.c:464`, `board/actions/implement.md`) still need changing, plus `plan.md`, which still names `plans/<slug>.md` and forbids commits. Worth not letting the git change feel like it closed that loop.

Want me to work this up as its own card? It touches `boardwork.c`, `boardfile.c` and all three action prompts, so it's a real change to mux rather than something I can fold into this one.
- : Card **`gss3`** — "CARD.md lives in git, not in a board-kept copy", on `/Users/jhickner/working/mux` (it resolved to the main checkout, not this worktree, via `gitcmd_root`'s worktree-list-first path). Queue is empty; nothing runs until you send it somewhere.

The write-up is in the card **body**, not a `plans/` file — so it's inlined into whatever action picks it up, which sidesteps the delivery gap the card itself is about. 67 lines covering:

- **The code**: delete `boardfile.{c,h}` and its four call sites; drop `ignore_card_file` (plus clean the repo-wide exclude line it already wrote); `write_card_file` always generates; and `drop_worktree` must refuse a dirty worktree, since deleting `boardfile_keep` removes the only thing standing between an uncommitted plan and `worktree remove --force` + `branch -D`.
- **The prompts**: all three actions, plus the false sentence at `boardwork.c:464`, with the note that these are compiled into `boarddefaults.c` and need a rebuild.
- **The traps**: the abandoned-card regression, cards without worktrees, and the fact that a re-made worktree recovers CARD.md from the branch for free.

Given the plan is already in the body, I'd send it straight to `implement` rather than `plan` — running the planner would just re-derive what's there. Your call.

Worth noting what this card does to the card we've been working: once it lands, `plan.md` will say the plan belongs in CARD.md and gets committed. This worktree's CARD.md already has the `## Plan` section but is still untracked and excluded — so when you're ready to hand `fngf` to an implementer, it'll need `git add -f CARD.md` until `gss3` removes that exclude line.
- : Yes — land `gss3` first. Three reasons, and one caveat that decides how you sequence it.

**They don't collide.** `gss3` touches `boardfile.{c,h}`, `boardwork.c`, `board.c` and the action prompts. `fngf` touches `matrix.c`, `cmd.c`, `workspace.c`, `session.c`, `sessionswitch.c` and a new `replbox`. No overlap, so no conflict risk either way.

**`fngf` has nothing to merge yet.** Zero commits on `worktree-fngf`, clean tree — the plan action produced no code. So there's no git-level ordering question at all; "come back to it" just means handing it to an implementer.

**And that hand-off is broken until `gss3` lands.** `fngf`'s plan is in CARD.md, which is untracked, excluded, and — worse — the implementer's prompt inlines the card *body* (the two original bullets) and explicitly says not to open the file. An implementer picking up `fngf` today builds the wrong thing. `gss3` is also small next to `fngf`: delete a module, four call sites, three prompts, one guard, versus a three-phase view rewrite. Better to have the big card run its whole length under the fixed convention than to fight the broken one for weeks.

**The caveat: merging `gss3` isn't sufficient — it has to be built and installed.** The action prompts are compiled into `src/boarddefaults.c` from `board/actions/*.md`, so the running mux keeps the old prompts until the binary is replaced. `make install` handles it, including the `pkill -SIGURG -a -x mux` restart signal that's mux's own reload path. So the order is: implement `gss3` → merge → `make install` → then hand `fngf` off.

**If you'd rather not serialize**, there's a zero-dependency alternative for `fngf`: move its plan from CARD.md into the card *body*, which is already inlined into the implementer's first turn with no code change — that's exactly what I did for `gss3`. Then `fngf` can go now and `gss3` lands on its own schedule. It costs you the git history on the plan, which is precisely what `gss3` exists to fix.

My recommendation: `gss3` first, then install, then `fngf`. Want me to queue `implement` on `gss3`? Given the plan is already in its body, I'd skip the `plan` action.
