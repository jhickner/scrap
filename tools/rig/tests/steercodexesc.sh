#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40 -- -b codex); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 20; echo done' 'sleep 20'
$R say $n 'do this instead' 'steer · sent.* · do this instead'
$R send $n Escape
$R wait $n 'codex: do this instead' 15
$R idle $n
screen=$($R snap $n)
[ "$(grep -c '^▌ do this instead$' <<<"$screen")" = 1 ] || { echo "steercodexesc: want the steer shown once" >&2; echo "$screen" >&2; exit 1; }
echo "steercodexesc: ok"
