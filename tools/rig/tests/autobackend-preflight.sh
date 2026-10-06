#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":20,"grok":20}'\'' > backend-quota.json' 'ran:'
$R say $n 'quota: 94' 'quota 94'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'below-limit' 'echo: below-limit'
$R say $n '/autobackend 94' 'auto-backend at 94%'
$R say $n 'next-request' 'codex: next-request'
