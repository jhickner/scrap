#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -- --model claude-opus-5-5 --effort xhigh); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: echo '\''{"codex":94,"grok":20}'\'' > backend-quota.json' 'ran:'
$R say $n 'history-marker' 'echo: history-marker'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95' 'codex: continue'
$R wait $n 'gpt-6-astra, xhigh.*history=kept'
