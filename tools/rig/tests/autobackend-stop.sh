#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":95,"grok":100}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95 wait' '⠋|⠙|⠹|thinking'
$R type $n 'queued-marker'; $R send $n Enter
$R wait $n 'auto-backend stopped at 95%'
$R say $n '/autobackend off' 'auto-backend off'
$R say $n '/status' 'claude'
$R send $n Up
$R wait $n '❯ queued-marker'
$R send $n Enter
$R wait $n 'echo: queued-marker'
