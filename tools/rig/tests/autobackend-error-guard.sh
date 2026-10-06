#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":20,"grok":20}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'discuss usage limit' 'echo: discuss usage limit'
$R say $n 'other-error' 'connection failed'
$R say $n 'still-here' 'echo: still-here'
