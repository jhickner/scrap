#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":"fail","grok":"fail"}'\'' > backend-quota.json' 'ran:'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95' 'auto-backend stopped at 95%'
$R say $n '/autobackend off' 'auto-backend off'
$R say $n 'still-working' 'echo: still-working'
