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
$R relay $a '[
  {"wait": "\"t\":\"busy\""},
  {"t": "line", "text": "run: sleep 3"}, {"wait": "\"on\":true"},
  {"t": "line", "text": "while running"}, {"wait": "echo: while running", "secs": 5}
]' >/dev/null
$R say $a 'run: sleep 3; echo late'
$R relay $a '[{"wait": "\"t\":\"user\",\"text\":\"run: sleep 3; echo late\"", "secs": 2}]' >/dev/null
$R relay $a '[{"t": "line", "text": "look", "files": [{"name": "a b.txt", "data": "aGVsbG8gZmlsZQ=="}]},
  {"wait": "I sent a file:\\\\n- /tmp/scrap_relay_[^ ]*-a_b.txt\\\\n\\\\nlook", "secs": 5}]' >/dev/null
f=$($R snap $a | grep -o '/tmp/scrap_relay_[^ ]*-a_b.txt' | head -1)
[ "$(cat "$f")" = "hello file" ] || { echo "relay: upload not saved: $f"; exit 1; }
$R idle $a
$R send $a C-d
$R wait $a 'echo: second tab' >/dev/null && sleep 1
if $R relay $a '[]' >/dev/null 2>&1; then echo "relay: still on after its tab closed"; exit 1; fi
echo "relay: ok"
