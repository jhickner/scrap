#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":10,"grok":10}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 94 wait' '⠋|⠙|⠹|thinking'
$R send $n Escape
$R wait $n 'interrupted: sleep 2'
$R idle $n
if $R snap $n | grep -q 'switched to'; then exit 1; fi
