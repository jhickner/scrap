#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n '/memory on' 'memory on'
$R say $n 'run: sleep 4; echo done' '\[bash\] sleep 4'
$R say $n 'go left' 'steer · next tool step · go left'
$R wait $n 'steered: go left' 15
$R idle $n
log=$(cat "$($R dir $n)"/state/config/memory/main/*.jsonl)
echo_at=$(grep -n '"kind":"echo"' <<<"$log" | tail -1 | cut -d: -f1)
user_at=$(grep -n '"kind":"user","text":"go left"' <<<"$log" | cut -d: -f1)
talk_at=$(grep -n '"kind":"talk","text":"steered: go left"' <<<"$log" | cut -d: -f1)
[ -n "$echo_at" ] && [ -n "$user_at" ] && [ -n "$talk_at" ] && [ "$echo_at" -lt "$user_at" ] && [ "$user_at" -lt "$talk_at" ] ||
    { echo "steermem: want echo, then the steer as user, then talk" >&2; echo "$log" >&2; exit 1; }
echo "steermem: ok"
