#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --fixture fixtures/ask.jsonl); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'pick' 'esc dismiss'
asker=$($R tab $n)
$R send $n C-n
$R wait $n 'session 2/2'
[ "$($R tab $n)" != "$asker" ]
$R send $n S-Up
$R idle $n
[ "$($R tab $n)" = "$asker" ]
$R wait $n 'esc dismiss'
