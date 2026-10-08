#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4; echo done' '\[bash\] sleep 4'
$R type $n 'after the turn'
$R send $n M-Enter
$R wait $n '^▌ after the turn$'
$R snap $n | grep -q 'steer ·' && { echo "steerqueue: alt-enter steered" >&2; $R snap $n >&2; exit 1; }
$R wait $n 'echo: after the turn' 15
$R idle $n
[ "$($R snap $n | grep -cE '^[0-9]+s( · |$)')" = 2 ] || { echo "steerqueue: want a turn of its own" >&2; $R snap $n >&2; exit 1; }
echo "steerqueue: ok"
