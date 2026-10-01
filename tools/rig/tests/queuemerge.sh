#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4' 'thinking'
$R say $n 'one' '▌ one'
$R type $n 'two'
$R send $n Enter
sleep 0.5
$R wait $n 'echo: one'
$R idle $n
! $R snap $n | grep -q 'echo: two'
