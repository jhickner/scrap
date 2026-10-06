#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake)
trap '$R stop $n' EXIT
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' $((20000 + RANDOM % 20000)) > "$($R dir $n)/state/config/relay"
$R wait $n '❯'
$R say $n 'quota: 25' 'quota 25'
$R say $n '/relay on' 'relay on ws'
$R relay $n '[
  {"wait":"\"t\":\"view\".*\"quota\":\\{\"used_percent\":25,\"resets_at\":2000000000,\"window_minutes\":300\\}"},
  {"quiet":"\\[\"session\",", "secs":2.2},
  {"t":"req","op":"send","text":"quota: 100"},
  {"wait":"\\[\"session\",.*\"used_percent\":100"},
  {"check":true},
  {"reconnect":true},
  {"wait":"\"t\":\"view\".*\"used_percent\":100"},
  {"t":"req","op":"new"},
  {"wait":"\"t\":\"view\".*\"used_percent\":100"},
  {"check":true},
  {"t":"req","op":"open","tab":0},
  {"wait":"\"t\":\"view\".*\"used_percent\":100"},
  {"t":"req","op":"send","text":"/backend codex"},
  {"wait":"\"backend\":\"codex\".*\"quota\":null"},
  {"check":true}
]' >/dev/null
