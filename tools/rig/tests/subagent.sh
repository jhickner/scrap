#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo)
trap '$R stop $n 2>/dev/null || true' EXIT
$R wait $n '❯'
$R say $n '/memory on' 'memory on'
me=$($R cli $n ls | awk 'NR==1{print $1}')
$R say $n "spawn: run: scrap send $me SUB\$((6*7))" 'echo: SUB42' 20
$R cli $n ls | grep -q 'agent: run: scrap send'
chat="$($R dir $n)/state/config/memory/main"
grep -q '"kind":"tool","text":"agent' "$chat"/*.jsonl
if grep -q '"kind":"tool","text":"bash' "$chat"/*.jsonl; then
    echo "subagent: its steps reached memory" >&2
    exit 1
fi
echo "subagent: ok"
