# What mux tells tmux-agent-tabs

A plan, not shipped behaviour. `agenttabs.c` is the whole of what mux publishes
to tmux-agent-tabs today; this is what it gets wrong and what to change.

## The contract

tmux-agent-tabs reads `<state_dir>/agents/*.json`. One record is one line of
JSON carrying `agent`, `pid`, `status`, `ts`, and optionally `tmux_pane`, the
`usage_*` fields and `provider`. The file name is free-form — every consumer
takes the pid from the record body, and `codex-<session>.json` already proves
it — so a record is identified by what is in it, not by what it is called.

Two consumers read those records:

- `scripts/tab-icon` paints the tab. It joins records to windows by
  `tmux_pane`, drops a record whose pid is dead or was recycled after the write,
  and where several records land on one window the strongest status wins
  (`needs-permission` > `errored` > `working` > `finished`).
- `scripts/usage` draws the status-right segments. A provider gets a segment
  only while it has an open session: `claude` and `grok` from a live record
  naming that agent, `codex` from `usage_percent`/`usage_resets_at`/`usage_ts`
  carried on a live `codex` record, OpenRouter from a live `pi` record whose
  `provider` is exactly `openrouter`. The network refresh behind the claude and
  grok segments only runs while some record in the tmux session reads
  `working`.

## What breaks

The record is process-wide. `agent`, `record`, `current_status` and the usage
fields are file statics in `agenttabs.c`, `agenttabs_begin()` is called once
from `main.c:530` with the backend named on the command line, and the path is
`agents/<pid>.json`. A mux window is not one session: `workspace.c` holds up to
`WORKSPACE_MAX` of them, each with its own backend.

1. **Only the launch backend is ever named.** A mux started as claude that
   opens a codex tab publishes nothing for codex — no codex segment, ever —
   and keeps claiming claude after the claude tab is closed.

2. **Status flaps.** `pump()` in `workspace.c` pumps every tab on every tick and
   `publish()` in `session.c` writes the one shared record from each, so the
   last tab pumped wins: an idle tab overwrites a working tab's `working` within
   the same tick. That is a spinner that drops out, a `working`→`finished` edge
   that lights the unread dot and chimes on a tab nobody finished, and — because
   `usage` gates its refresh on some record reading `working` — a usage segment
   that stops being refreshed while work is running.

3. **`/backend` is invisible.** `cmd.c:308` switches a live session to another
   backend; `agent` in the record keeps the old name for the life of the
   process.

4. **Codex usage lands on the wrong record.** `agenttabs_usage()` keeps one
   percent per process and writes it into a record labelled with the launch
   backend, so `codex_open()` finds nothing unless mux was launched `-b codex`,
   and a codex tab's quota can be printed against claude's mark.

5. **pi never says `openrouter`.** mux passes no provider to the pi driver and
   writes no `provider` field, so the OpenRouter balance segment never appears
   for a mux pi session.

6. **Records leak on a signal.** Cleanup is `atexit(drop_record)`; `on_fatal()`
   in `tty.c` restores the terminal and re-raises, so SIGHUP from a closed pane
   and SIGTERM skip it. Consumers survive this on the pid and recycled-pid
   checks, but mux leaves its own litter in `agents/`.

7. **Publishing gives up silently.** `agenttabs_begin()` returns early when
   `TMUX_PANE` is unset or the state directory does not exist yet, and nothing
   retries: a mux started before the plugin first created the directory
   publishes nothing for as long as it runs.

## The plan

**Per-session records.** Rework `agenttabs.c` along the lines of `livelist.c`,
which already solves this for mux's own session list: a slot table keyed by the
`struct session *`, a record per slot at `agents/<pid>-<slot>.json`, and
`drop_all()` over the table. The new entry point is

    void agenttabs_publish(const struct session *s, const char *status);

called from `publish()` in `session.c` next to `livelist_publish()`, with
`agent` read from `session_backend(s)` at write time — which is what fixes
`/backend` as well. `agenttabs_working()`/`_finished()`/`_errored()` go away.
`agenttabs_begin()` keeps arming the directory, setting `AGENT_TABS_WRAPPED`
and registering the cleanup. Session close calls `agenttabs_forget()`, the
counterpart of `livelist_forget()`, so a closed tab stops claiming its backend
immediately rather than at process exit. No change is needed downstream:
several records per pane already resolve by strongest status.

**Usage per session.** `agenttabs_usage()` takes the session, so the codex
percent is written on that session's own record. `usage_poll()` in `session.c`
already runs per session and needs only to pass `s` through.

**pi provider.** Publish `provider` from the session's pi provider and pass it
to the driver (`pi.h` takes `--provider`), so an OpenRouter-routed pi tab
renders its balance.

**Signal-safe cleanup.** Hook the record drop into the fatal path in `tty.c`
rather than `atexit` alone — `unlink()` is async-signal-safe, so `on_fatal()`
can drop mux's own records before it re-raises. `livelist` has the same hole
and should share the hook.

**Retry the arm.** When `agenttabs_begin()` finds no state directory, leave the
publisher armed but idle and re-check on the next publish rather than
disabling it for the process. `TMUX_PANE` staying unset is a real "not under
tmux" and can keep bailing out.

## Testing it

With mux under tmux, open a claude tab and a codex tab in one window
(`/sessions`, `^n`), then watch `~/.config/tmux/tmux-agent-tabs/agents/`: there
should be one record per tab, each naming its own backend, and a turn on either
should leave the other's status alone. The status-right should show both marks,
with codex's percent from its own record. `/backend codex` on a claude tab
should move that record's `agent` within a tick, `^x` should delete it, and
`tmux kill-pane` should leave nothing behind.
