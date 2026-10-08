#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40 -- -b grok); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4; echo done' 'sleep 4'
$R say $n 'notsteerable now' 'steer · after this turn · notsteerable now'
$R wait $n 'ran: done' 15
$R wait $n 'grok: notsteerable now' 15
$R idle $n
[ "$($R snap $n | grep -cE '^[0-9]+s( · |$)')" = 2 ] || { echo "steergrokrefuse: want the refused steer as the next turn" >&2; $R snap $n >&2; exit 1; }
echo "steergrokrefuse: ok"
