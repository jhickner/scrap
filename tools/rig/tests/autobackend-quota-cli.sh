#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":20,"grok":20}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
d=$($R dir $n)
(cd "$d/work" && PATH="$d/bin:$PATH" "$d/bin/scrap" --state "$d/state" -b claude -p 'quota-error history-marker') >"$d/cli-output" 2>&1
cat "$d/cli-output"
grep -q 'codex: continue.*history=kept' "$d/cli-output"
