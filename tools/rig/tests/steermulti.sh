#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 5; echo done' '\[bash\] sleep 5'
$R say $n 'first a1' 'steer · next tool step · first a1'
$R say $n 'then b2' 'steer · next tool step · then b2'
$R wait $n 'steered: first a1 \| then b2' 15
$R idle $n
[ "$($R snap $n | grep -cE '^[0-9]+s( · |$)')" = 1 ] || { echo "steermulti: want one turn" >&2; $R snap $n >&2; exit 1; }
echo "steermulti: ok"
