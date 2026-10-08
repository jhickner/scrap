#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 5; echo done' '\[bash\] sleep 5'
$R say $n 'typo here' 'steer · next tool step · typo here'
$R send $n Up
$R wait $n '❯ typo here'
$R snap $n | grep -q 'steer ·' && { echo "steerrecall: row left after recall" >&2; $R snap $n >&2; exit 1; }
$R wait $n 'tool said: done' 15
$R snap $n | grep -q 'steered:' && { echo "steerrecall: recalled steer was delivered" >&2; $R snap $n >&2; exit 1; }
echo "steerrecall: ok"
