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
