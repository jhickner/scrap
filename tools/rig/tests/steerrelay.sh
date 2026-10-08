#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' \
    $((20000 + RANDOM % 20000)) > "$($R dir $n)/state/config/relay"
$R wait $n '❯'
$R say $n '/relay on' 'relay on ws'

$R say $n 'run: sleep 4; echo done' '\[bash\] sleep 4'
$R relay $n '[
  {"wait": "\"t\":\"view\""},
  {"t": "req", "op": "send", "text": "go left", "id": "s1"},
  {"wait": "\"id\":\"s1\",\"ok\":true"},
  {"wait": "\"kind\":\"user\"[^}]*go left", "secs": 15}
]' >/dev/null
$R wait $n 'steered: go left' 15
$R idle $n
screen=$($R snap $n)
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 1 ] || { echo "steerrelay: steer wanted one turn" >&2; echo "$screen" >&2; exit 1; }

$R say $n 'run: sleep 4; echo again' '\[bash\] sleep 4; echo again'
$R relay $n '[
  {"wait": "\"t\":\"view\""},
  {"t": "req", "op": "send", "text": "after it", "mode": "queue", "id": "q1"},
  {"wait": "\"id\":\"q1\",\"ok\":true"}
]' >/dev/null
$R snap $n | grep -q 'steer ·' && { echo "steerrelay: queue mode steered" >&2; $R snap $n >&2; exit 1; }
$R wait $n 'echo: after it' 20
$R idle $n
screen=$($R snap $n)
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 3 ] || { echo "steerrelay: queue wanted a turn of its own" >&2; echo "$screen" >&2; exit 1; }
$R say $n 'run: sleep 5; echo third' '\[bash\] sleep 5; echo third'
$R relay $n '[
  {"wait": "\"t\":\"view\""},
  {"t": "req", "op": "send", "text": "drop me", "id": "d1"},
  {"wait": "\"queue\":\\[\"drop me\"\\],\"items\":\\[\\{\"text\":\"drop me\",\"state\":\"steer\"\\}\\]"},
  {"t": "req", "op": "unqueue", "text": "drop me", "id": "u1"},
  {"wait": "\"id\":\"u1\",\"ok\":true"},
  {"t": "req", "op": "send", "text": "after third", "mode": "queue", "id": "q2"},
  {"wait": "\"items\":\\[\\{\"text\":\"after third\",\"state\":\"queued\"\\}\\]"}
]' >/dev/null
$R wait $n 'echo: after third' 20
$R idle $n
$R snap $n | grep -q 'steered: drop me' && { echo "steerrelay: the unqueued steer landed" >&2; $R snap $n >&2; exit 1; }
echo "steerrelay: ok"
