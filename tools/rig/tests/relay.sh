#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
a=$($R start --fake)
trap '$R stop $a' EXIT
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' \
    $((20000 + RANDOM % 20000)) > "$($R dir $a)/state/config/relay"
$R wait $a '❯'
$R say $a '/relay on' 'relay on ws'
$R relay $a '[
  {"wait": "\"t\":\"busy\""},
  {"t": "line", "text": "/open"}, {"wait": "\"t\":\"idle\""},
  {"t": "line", "text": "run: sleep 3", "tab": 2}, {"wait": "\"on\":true"},
  {"t": "pick", "payload": "#0"}, {"wait": "\"index\":1,[^}]*\"current\":true", "secs": 0.5},
  {"t": "line", "text": "while two runs", "tab": 1}, {"wait": "echo: while two runs", "secs": 2}
]' >/dev/null
echo "relay: ok"
