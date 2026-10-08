#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40 -- -b codex); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 5; echo done' 'sleep 5'
$R say $n 'first a1' 'steer · .* · first a1'
$R say $n 'then b2' 'steer · .* · then b2'
$R wait $n 'steered: first a1 \| then b2' 15
$R idle $n
[ "$($R snap $n | grep -cE '^[0-9]+s( · |$)')" = 1 ] || { echo "steercodexmulti: want one turn" >&2; $R snap $n >&2; exit 1; }
echo "steercodexmulti: ok"
