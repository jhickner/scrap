#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":95,"grok":100}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95 wait' 'auto-backend stopped at 95%'
$R wait $n 'interrupted: sleep 2'
$R idle $n
if $R snap $n | grep -q 'switched to'; then exit 1; fi
$R say $n '/autobackend off' 'auto-backend off'
$R say $n 'still-here' 'echo: still-here'
