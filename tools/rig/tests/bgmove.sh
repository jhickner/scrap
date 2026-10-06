#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fixture fixtures/bgmove.jsonl); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n 'wait'
$R send $n Enter
$R wait $n 'Moved to the background'
$R wait $n '\[bash ↗\] sleep 99'
$R wait $n 'bash +sleep 99'
echo "bgmove: ok"
