#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40 -- -b grok); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4; echo done' 'sleep 4'
$R say $n 'go left' 'steer · sent.* · go left'
$R wait $n 'steered: go left' 15
$R idle $n
screen=$($R snap $n)
grep -q '^▌ go left$' <<<"$screen" || { echo "steergroksingle: no user block for the steer" >&2; echo "$screen" >&2; exit 1; }
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 1 ] || { echo "steergroksingle: want one turn" >&2; echo "$screen" >&2; exit 1; }
grep -q '"method": "_x.ai/interject"' "$($R dir $n)/work/backend-protocol.jsonl" || { echo "steergroksingle: no interject sent" >&2; exit 1; }
$R say $n 'idle now' 'grok: idle now'
[ "$(grep -c '"method": "_x.ai/interject"' "$($R dir $n)/work/backend-protocol.jsonl")" = 1 ] || { echo "steergroksingle: interject sent outside the steer" >&2; exit 1; }
echo "steergroksingle: ok"
