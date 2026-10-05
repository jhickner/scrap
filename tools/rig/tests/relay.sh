#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
a=$($R start --fake)
trap '$R stop $a' EXIT
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' \
    $((20000 + RANDOM % 20000)) > "$($R dir $a)/state/config/relay"
$R wait $a '❯'
$R say $a '/new second tab' 'echo: second tab'
$R say $a '/relay on' 'relay on ws'

# view, request ids, tagged prompt, mid-turn line, replica == fresh view
$R relay $a '[
  {"wait": "\"t\":\"hello\".*\"theme\":\\{\"background\":\"#[0-9a-f]{6}\",\"roles\":\\{\"input\":\\{\"fg\""},
  {"wait": "\"t\":\"view\".*\"binding\":0.*\"hud\":\\[\\[\\{\"text\""},
  {"t": "req", "op": "send", "text": "run: sleep 2"},
  {"wait": "\"t\":\"res\",\"id\":\"r1\",\"ok\":true"},
  {"wait": "\"kind\":\"user\",\"ts\":[0-9.]+,\"text\":\"run: sleep 2\",\"req\":\"r1\""},
  {"wait": "\"busy\":true"},
  {"t": "req", "op": "send", "text": "while running"},
  {"wait": "echo: while running", "secs": 6},
  {"wait": "\"kind\":\"end\",\"ts\":[0-9.]+,\"secs\":[0-9.]+"},
  {"wait": "\"busy\":false"},
  {"check": true}
]' >/dev/null

# a prompt typed at the terminal shows up as a user entry
$R say $a 'run: sleep 1; echo late'
$R relay $a '[{"wait": "\"kind\":\"user\",\"ts\":[0-9.]+,\"text\":\"run: sleep 1; echo late\"", "secs": 3}]' >/dev/null
$R idle $a

# uploads
$R relay $a '[{"t": "req", "op": "send", "text": "look", "files": [{"name": "a b.txt", "data": "aGVsbG8gZmlsZQ=="}]},
  {"wait": "- /tmp/scrap_relay_[^ ]*-a_b.txt\\\\n\\\\nlook", "secs": 5}]' >/dev/null
f=$($R snap $a | grep -o '/tmp/scrap_relay_[^ ]*-a_b.txt' | head -1)
[ "$(cat "$f")" = "hello file" ] || { echo "relay: upload not saved: $f"; exit 1; }
$R idle $a

# tool call with its result held back until fetched; file request
$R relay $a '[
  {"t": "req", "op": "send", "text": "tool: echo hi"},
  {"wait": "\"kind\":\"tool\".*\"name\":\"bash\".*\"runs\":\\[\\[\"echo\",\"syntax_command\"\\]"},
  {"wait": "\\[\"set\",[0-9]+,\\{\"done\":true,\"held\":true\\}\\]"},
  {"wait": "tool done"},
  {"check": true},
  {"t": "req", "op": "file", "path": "'"$f"'", "id": "f1"},
  {"wait": "\"id\":\"f1\",\"ok\":true.*\"data\":\"aGVsbG8gZmlsZQ==\""},
  {"t": "req", "op": "file", "path": "/nonexistent", "id": "f2"},
  {"wait": "\"id\":\"f2\",\"ok\":false,\"code\":\"not_found\""},
  {"t": "req", "op": "send", "text": "", "id": "b1"},
  {"wait": "\"kind\":\"hud\".*\"hud\":\\[\\[\\{\"text\":\"▌ \""},
  {"wait": "\"id\":\"b1\",\"ok\":true"},
  {"t": "req", "op": "highlight", "lang": "py", "text": "def f(): pass", "id": "h1"},
  {"wait": "\"id\":\"h1\",\"ok\":true,\"runs\":\\[\\[\"def\",\"syntax_keyword\"\\]"},
  {"t": "req", "op": "highlight", "lang": "nope", "text": "x", "id": "h2"},
  {"wait": "\"id\":\"h2\",\"ok\":true}"}
]' >/dev/null
$R idle $a

# ask form published and answered
$R relay $a '[
  {"t": "req", "op": "send", "text": "askme"},
  {"wait": "\\[\"ask\",\\{\"id\":1,\"questions\":\\[\\{\"text\":\"Color\\?\""},
  {"t": "req", "op": "answer", "ask": 1, "choice": [1], "text": [""]},
  {"wait": "echo: 1. blue"},
  {"check": true},
  {"t": "req", "op": "answer", "ask": 1, "choice": [0], "id": "late"},
  {"wait": "\"id\":\"late\",\"ok\":false,\"code\":\"stale\""}
]' >/dev/null
$R idle $a

# resuming after missing a turn replays the deltas instead of a view
$R relay $a '[
  {"wait": "\"t\":\"view\""},
  {"t": "req", "op": "send", "text": "run: sleep 1; echo missed"},
  {"resume": 2.5},
  {"wait": "\"t\":\"resumed\""},
  {"check": true},
  {"count": "ran: missed", "n": 1}
]' >/dev/null
$R idle $a

# the same send id twice runs once, also across a reconnect
$R relay $a '[
  {"t": "req", "op": "send", "text": "once", "id": "dup"},
  {"wait": "\"id\":\"dup\",\"ok\":true"},
  {"reconnect": true},
  {"t": "req", "op": "send", "text": "once", "id": "dup"},
  {"wait": "\"id\":\"dup\",\"ok\":true"},
  1,
  {"count": "echo: once", "n": 1}
]' >/dev/null

# reconnect mid-turn keeps the turn so far; stop is not stuck behind a queued line
$R relay $a '[
  {"t": "req", "op": "send", "text": "tool: sleep 1; echo slow"},
  {"wait": "\"kind\":\"tool\""},
  {"reconnect": true},
  {"wait": "\"t\":\"view\".*\"kind\":\"tool\".*sleep 1; echo slow"},
  {"wait": "tool done", "secs": 5},
  {"check": true},
  {"t": "req", "op": "send", "text": "run: sleep 2"},
  {"wait": "\"busy\":true"},
  {"t": "req", "op": "send", "text": "after stop", "id": "q"},
  {"t": "req", "op": "stop", "id": "s"},
  {"wait": "\"id\":\"s\",\"ok\":true", "secs": 2},
  {"wait": "\"kind\":\"end\".*\"stopped\":true", "secs": 5},
  {"wait": "echo: after stop", "secs": 5}
]' >/dev/null
$R idle $a

# two clients both get the turn
$R relay $a '[{"wait": "echo: both", "secs": 8}]' >/dev/null & bg=$!
sleep 1
$R relay $a '[{"t": "req", "op": "send", "text": "both"}, {"wait": "echo: both"}]' >/dev/null
wait $bg
$R idle $a

# /relay on in another tab rebinds without dropping the client
$R relay $a '[{"wait": "\"t\":\"view\".*\"binding\":1", "secs": 8},
  {"t": "req", "op": "send", "text": "x", "binding": 0, "id": "old"},
  {"wait": "\"id\":\"old\",\"ok\":false,\"code\":\"stale\""}]' >/dev/null & bg=$!
sleep 1
$R say $a '/new third tab' 'echo: third tab'
$R say $a '/relay on' 'relay'
wait $bg

# the /sessions list, and opening a row moves the relay to it
$R relay $a '[
  {"t": "req", "op": "sessions", "id": "ls"},
  {"wait": "\"id\":\"ls\",\"ok\":true,\"rows\":.*\"kind\":\"tab\".*\"tab\":2,\"relay\":true"},
  {"t": "req", "op": "open", "tab": 1, "id": "o"},
  {"wait": "\"t\":\"view\".*\"binding\":2.*echo: second tab"},
  {"wait": "\"id\":\"o\",\"ok\":true,\"binding\":2"},
  {"t": "req", "op": "sessions", "id": "ls2"},
  {"wait": "\"id\":\"ls2\".*\"label\":\"▸ [^\"]*\",[^}]*\"tab\":1,\"relay\":true"},
  {"t": "req", "op": "open", "target": "@nobody", "id": "o2"},
  {"wait": "\"id\":\"o2\",\"ok\":false,\"code\":\"not_found\""},
  {"t": "req", "op": "new", "id": "n"},
  {"wait": "\"id\":\"n\",\"ok\":true,\"binding\"", "secs": 15},
  {"t": "req", "op": "open", "tab": 1, "id": "o3"},
  {"wait": "\"id\":\"o3\",\"ok\":true,\"binding\"", "secs": 15}
]' >/dev/null
$R send $a C-d
sleep 1

$R send $a C-d
$R wait $a 'echo: second tab' >/dev/null && sleep 1
$R relay $a '[{"wait": "\"t\":\"view\""}]' >/dev/null
$R send $a C-d
sleep 1
if $R relay $a '[]' >/dev/null 2>&1; then echo "relay: still on after its tab closed"; exit 1; fi
echo "relay: ok"
