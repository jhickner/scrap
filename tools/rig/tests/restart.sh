#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n '/new second tab'
$R send $n Enter
$R wait $n 'session 2/2'
$R wait $n 'echo: second tab'
names=$($R snap $n | head -4 | grep -Eo '@[a-z]+(-[0-9]+)?')
$R type $n /restart
$R send $n Enter
$R wait $n 'restarted 1x'
for name in $names; do $R wait $n "$name"; done
for _ in $(seq 50); do [ -z "$(ls "$($R dir $n)/state/tmp")" ] && break; sleep 0.1; done
[ -z "$(ls "$($R dir $n)/state/tmp")" ] || { echo "restart left files in state/tmp" >&2; exit 1; }
