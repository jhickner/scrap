#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo)
trap '$R stop $n 2>/dev/null || true' EXIT
$R wait $n '❯'
$R say $n '/memory on' 'memory on'
$R say $n 'spawn: run: echo SUB$((6*7))' 'tool said: tool said: SUB42' 20
$R cli $n ls --live | grep -q 'agent: run: echo SUB'
chat="$($R dir $n)/state/config/memory/main"
grep -q '"kind":"tool","text":"agent' "$chat"/*.jsonl
if grep -q '"kind":"tool","text":"bash' "$chat"/*.jsonl; then
    echo "subagent: its steps reached memory" >&2
    exit 1
fi
echo "subagent: ok"
