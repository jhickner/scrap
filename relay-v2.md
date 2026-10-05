# Relay v2

Sources: pi `packages/durable` plus the stack it rides on (`pi-protocol`, `pi-server`,
`pi-client`, Chord services); scrap relay as of 05e30a6 (one tab).

## 1. What pi does

- Clients see only committed state. Streaming tokens are durable writes every 100 ms.
- Protocol: versioned `hello`, then `request{id,target,call}` / `response{id,ok,result|error}` /
  `cancel{id}`, plus `service_update{subscriptionId,update}`. Every request gets exactly one
  response.
- Observation is **state replication, not an event log**. The client subscribes, the response
  is a full snapshot of a JSON view `{entries, docs:{live, inbox, agent, usage}}`, and
  contiguous `sequence`-numbered op batches follow (`r` replace, `s` set, `d` delete,
  `a` append string, `t` trim, `p` splice).
- If a sequence has a gap, the client fails and resubscribes. More than 100 pending
  updates are collapsed into one `reset` snapshot. Nothing is replayed and nothing
  persists across disconnects.
- Stable ids: entries are immutable and globally numbered. Live run state lives in a
  `live` document: `run`, the partial `generation.message`, and tool slots with running
  output, `status` and `entry` link.
- Commands: `prompt` (rejected if busy), `steer` (placed at the next tool boundary),
  `followUp` (placed at turn end), `cancelQueued(entryId)`, `abort`, `waitForPrompt(opId)`.
  The queue is visible as `inbox` state.
- Fencing: `serverId` is checked at hello, and a server-issued `attachmentId` on every
  session request rejects stale frames after a rebind.
- Multi-client: N subscribers share one view. Writes are serialized; there is no presence.
- Weak spots, not worth copying: prompt idempotency exists (`requestId`) but isn't
  wired to the wire API, the protocol has no heartbeat, and there's no history paging.

## 2. Gaps in our relay

| # | Gap | Effect today |
|---|-----|--------------|
| 1 | No request ids or responses | `more` with a bad id never replies; `stop` and `hello` are unacked; `idle` can't be matched to a line |
| 2 | Turns identified by array index | `more` after reload or `/clear` duplicates or skips turns |
| 3 | Reconnect loses the running turn | replies and tools already sent are gone; only the prompt is re-mirrored |
| 4 | No sequence numbers | client can't detect a dropped frame |
| 5 | History is lossy | no tools, no tool results, no thinking, 6 KB clip, 4 KB batches |
| 6 | Busy and queue are opaque | `busy` is polled; queued lines and steer/btw routing are invisible; `done` is suppressed while lines are queued |
| 7 | Dedupe of mirrored prompts by `strstr(rt.sent)` | can hide an unrelated terminal prompt |
| 8 | Inbox head-of-line blocking | `stop` stuck behind a `line` mid-turn |
| 9 | Blocking send on the UI thread with a 10 s timeout; no server ping | half-open phone freezes the TUI |
| 10 | Single client, newest wins | two phones evict each other forever |
| 11 | Rebinding to a tab drops the socket | client must notice and re-hello |
| 12 | Ask form closes on any relay frame | phone can't see or answer the form natively |
| 13 | Silent drops | inbox full, oversized frame, upload failure, unknown `t` |
| 14 | Housekeeping | uploads never deleted, image symlinks pile up, `bind=*` gives `127.0.0.1` URLs, token in every image URL, non-constant-time compare |
| 15 | No protocol version | old and new apps can't tell they're mismatched |

## 3. Protocol (implemented)

The server is `src/relay.c` on `src/vendor/wsd.h`, with JSON text frames over
`ws://<tailnet ip>:<port>/`. Auth is `Authorization: Bearer <token>` or
`?token=` from `~/.config/scrap/relay`. The connection carries no TLS; tailnet
traffic is already encrypted by WireGuard. The token compare is constant-time.

### 3.1 Frames

```
C→S {t:"hello", v:2, client:"<uuid, stable per install>",
     resume?:{server, seq, binding}}                              first frame
S→C {t:"hello", v:2, server:"<per process>", name:"scrap",
     theme:{background, roles:{<role>:{fg, wash?, style?}}}}
S→C {t:"error", code:"version", v:2}                              on a v mismatch
S→C {t:"view", seq, binding, session, entries, older, live, ask}  after hello and on rebind
S→C {t:"resumed", seq}                                           instead of a view; missed deltas follow
S→C {t:"delta", seq, ops:[op]}                                    seq = previous seq + 1
C→S {t:"req", id:"<client id>", op, binding?, ...args}
S→C {t:"res", id, ok:true, ...result} | {t:"res", id, ok:false, code, msg}
```

- **seq and resync:** `seq` is one counter shared by all clients. A `view` carries the
  current value and each delta increments it. On a gap, send `hello` again (or
  reconnect); the new `view` replaces all local state.
- **Resume:** a reconnecting client that kept its state sends `resume` with the hello's
  `server`, its `seq` and `binding`. If the server is the same process, the binding is
  unchanged and the last 256 deltas cover everything since `seq`, it answers `resumed`
  and replays them; otherwise it sends a `view`. Rebinds and server restarts force a view.
- **Rebind:** `/relay on` in another tab sends a new `view` with `binding + 1` on the
  same socket.
- **Liveness:** the server pings at the websocket level every 15 s and drops a client
  silent for 45 s. Up to 8 clients; the oldest is dropped when a 9th connects.
- **Sends never block the terminal:** each client has its own send queue, and a client
  whose queue passes 96 MB is dropped.

### 3.2 State

```
session: {id, title, cwd, backend, model, name, hud:[[{text, role}]]}  hud = scrap's 3 status rows; at most 1/s
entries: last 50, oldest first; older:true when more exist
live:    {busy, started?, context?, queue:[text]}
ask:     null | {id, questions:[{text, options:[{label, detail?}]}]}

entry:   {id, kind, ts, ...}   id: per-relay counter, never reused within a binding
  user       text, req?             req = the phone request that produced it
  assistant  text, images?:[path]   absolute paths; fetch with op "file"
  thinking   text
  tool       name, arg, input?(raw JSON text), then done, result?, diff?, failed?
             input, result and diff are not sent; held:true marks them, fetch with op "entry"
  note       text                   backend warnings
  btw        text, answer, failed?  /btw side-channel answers
  end        every turn: secs; stopped:true | failed:true, text = error
  any        clipped:true when a string field over 16 KB was cut; fetch with op "entry"
```

- **History from disk** has only user, assistant and stopped entries. Tool calls appear
  for turns run while the relay is on.
- **Tool results** carry no call id, so each one fills the oldest tool entry still open.

### 3.3 Delta ops

| op | meaning |
|----|---------|
| `["add", entry]` | append |
| `["set", id, fields]` | merge fields into an entry (tool results) |
| `["live", live]` | replace |
| `["ask", ask \| null]` | replace |
| `["session", session]` | replace |

### 3.4 Requests

| op | args | result | lane |
|----|------|--------|------|
| `send` | `text`, `files?:[{name, data(base64)}]` | `{}`; slash command output is added as a `note` entry | idle |
| `answer` | `ask`, `choice:[int, -1 = none]`, `text:[str]`, `reply?` | as `send` | idle |
| `stop` | none | `{}` | control |
| `unqueue` | `text` (a `live.queue` item) | `{}` / `not_found` | control |
| `older` | `before` (entry id), `limit?` (≤200, default 50) | `{entries, older}` | control |
| `entry` | `id` | `{entry}`, unclipped | control |
| `sessions` | none | `{rows:[{kind:head\|tab\|live, label, detail, target, id, when?, busy?, error?, tab?, relay?}]}`, the local /sessions list | control |
| `open` | `tab` or `target` | `{binding}`; a live row in another window is moved into this one first | idle |
| `file` | `path` | `{name, size, data(base64)}`, up to 32 MB | websocket thread |

- **Lanes:** the idle lane waits until the terminal is idle and no modal is open. Control
  requests run on arrival, even mid-turn and ahead of queued sends.
- **Idempotency:** the server remembers the last 64 results of `send`, `answer` and
  `unqueue`, keyed by (hello `client`, request `id`). A repeat gets the stored `res` and
  doesn't run again, which makes resending unanswered requests after a reconnect safe.
- **Binding check:** `send`, `answer` and `unqueue` with a `binding` that differs from
  the current one fail with `stale`.
- **Uploads:** at most 8 files, written to a temp dir that is removed when the relay
  stops. The text sent becomes `I sent a file:\n- <path>\n\n<text>`. Files that couldn't
  be saved are listed in `res.failed`.
- **Errors** are `invalid`, `stale`, `shell`, `refused`, `not_found`, `too_large`,
  `unreadable`, `full`, `hello`, `failed`. Nothing is dropped silently.
- **Routing:** a `send` while busy follows the terminal's rule (queue, redirect, or
  `/btw`). The result shows up in the view: `live.queue`, a new `user` entry, or a
  `btw` entry.

### 3.5 Client contract (Scrap, `ios-scrap`)

1. Connect, send `hello` with a stable `client` uuid, render `view`, then apply deltas in
   `seq` order. On a disconnect, reconnect with backoff (1 s → 30 s) and send `hello`
   with `resume`; on a gap, send it without.
2. Keep unanswered mutating requests (with their ids) locally and resend them after
   reconnecting.
3. Render from the view only. A pending local bubble is replaced by the `user` entry
   whose `req` matches its id.
4. Tool entries carry everything needed for both display modes (collapsed and compact),
   toggled in the app, as in scrap.

## 4. Not done

- **Token-level streaming:** backends merge text into whole blocks before the relay sees
  them, so it needs backend work first.
- **Tab or session switching from the phone:** add as an op that bumps `binding`.
- **Tool calls in disk-loaded history:** the transcript keeps only user and assistant text.

## 5. Tests

- `tools/rig/tests/relay.sh` uses `relayclient.py`, which keeps a copy of the state,
  fails on a seq gap, and has `check` (copy equals a fresh view), `count` and
  `reconnect` steps.
- **Covered:** view and request ids, prompt tagging, mid-turn send, terminal prompt,
  uploads, tool and result entries, `file`, ask publish and answer plus a stale
  answer, duplicate send id across a reconnect, reconnect mid-turn, stop ahead of a
  queued send, two clients, rebind plus a stale binding, and the relay going down
  when its tab closes.
- `fake-claude` has two test prompts: `tool: <cmd>` emits a Bash tool call and its
  result, and `askme` replies with an `@ask` block.

## 6. Scrap app design (proposal)

Font: 0xProto Nerd Font Mono, bundled with the app. The app should look like scrap in the terminal, as one monospace grid on scrap's
background. Colours are scrap's theme roles (`src/ui.c`), sent by the server, so the
app follows `~/.config/scrap` themes.

| role | default | used for |
|------|---------|----------|
| background | `#202746` | everything |
| brand | `#dfe2f1` | banner, bold, headings, code, tool label |
| body | `#979db4` | assistant text |
| dim | `#6b7394` | durations, hints, `@ask` source text |
| chrome | `#5e6687` | rules, boxes |
| input / input_echo | `#22a2c9` | prompt `❯`, cursor, user `▌` bar, `@name` |
| thinking | `#6679cc` italic | thinking entries |
| link | `#3d8fd1` underlined | links |
| ok | `#ac9739` | `+N` diff, success marks |
| error | `#c94922` | errors, `-N` diff, failed tools |

Screen, top to bottom:

```
[≡]                                   ╭──────────────╮
                                      │ @heaptronic-2 │
▌ ▄▀▀ ▄▀▀ █▀▄ ▄▀▄ █▀▄                 ╰──────────────╯
▌ ▄▄▀ ▀▄▄ █▀▄ █▀█ █▀           banner + status block, scrolled away like the terminal
▌ scrap › 0.942+db7bc87 › @heaptronic-2
▌ claude › opus-5-5[1m] › medium
▌ ~/working/apps/scrap on master · 3 in / 3 out

▌ hello there                   user entry: accent bar, brand text
echo: hello there               assistant: body colour, markdown
0s                              turn duration, dim

  [bash] ls /tmp | head -3      tool, compact; tap toggles the result/diff below it
  ⠦ 6s · thinking               spinner row while busy
─────────────────────────────────────────────
  queued: after stop       ×    live.queue, dim; × sends unqueue
❯ █                       [+] [↑]/[■]   composer: attach, send, stop while busy
─────────────────────────────────────────────
```

- **`[≡]` (top left)** opens a sheet in the style of `/sessions`. Rows are grouped by
  window and machine, and each shows `@name`, title, cwd and busy/unseen marks.
  Picking one moves that session into the relay's window as a tab (as `/sessions` does) and points the relay at it.
- **Ask forms** are drawn like the terminal form (`▌ 1 question`, `○` / `●` options,
  `✎ type an answer`, `↳ reply instead`), with taps instead of keys. Submitting sends
  `answer`.
- **Images** are fetched with `file` and shown inline. Tapping one opens it full screen.
- **Long entries** marked `clipped` show "… more", which fetches the whole entry with
  `entry`.
- **History:** scrolling to the top loads earlier entries with `older`.
- **Settings:** a long press on `[≡]` opens a sheet with the compact/expanded toggle and
  host, port and token.

The server side is done: the theme in `hello`, `session.hud`, `end.secs` on every turn,
and the `sessions` and `open` ops. The session list covers this machine only; other
tailnet machines aren't surveyed yet.
