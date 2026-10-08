#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 20; echo done' '\[bash\] sleep 20'
$R say $n 'do this instead' 'steer · next tool step · do this instead'
$R send $n Escape
$R wait $n '(echo|steered): do this instead' 10
$R idle $n
screen=$($R snap $n)
grep -q '^▌ do this instead$' <<<"$screen" || { echo "steeresc: no user block" >&2; echo "$screen" >&2; exit 1; }
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 2 ] || { echo "steeresc: want the steer as the next turn" >&2; echo "$screen" >&2; exit 1; }
echo "steeresc: ok"
