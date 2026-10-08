#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4; echo done' '\[bash\] sleep 4'
$R say $n 'go left' 'steer · sent.* · go left'
$R wait $n 'steered: go left' 15
$R idle $n
screen=$($R snap $n)
grep -q '^▌ go left$' <<<"$screen" || { echo "steerclaudesingle: no user block for the steer" >&2; echo "$screen" >&2; exit 1; }
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 1 ] || { echo "steerclaudesingle: want one turn" >&2; echo "$screen" >&2; exit 1; }
grep -q 'steer ·' <<<"$screen" && { echo "steerclaudesingle: steer row left behind" >&2; echo "$screen" >&2; exit 1; }
echo "steerclaudesingle: ok"
