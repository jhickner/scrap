#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":20,"grok":20}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota-error wait' '⠋|⠙|⠹|thinking'
$R send $n Escape
$R wait $n "You've hit your session limit"
$R idle $n
if $R snap $n | grep -q 'switched to'; then exit 1; fi
$R say $n 'still-here' 'echo: still-here'
