#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4' 'thinking'
$R type $n 'one'
$R send $n M-Enter
$R wait $n '▌ one'
$R type $n 'two'
$R send $n M-Enter
sleep 0.5
$R wait $n 'echo: one'
$R idle $n
! $R snap $n | grep -q 'echo: two'
