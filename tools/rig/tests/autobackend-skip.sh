#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":95,"grok":94}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95' 'grok: continue'
$R wait $n 'grok-4.7, medium'
