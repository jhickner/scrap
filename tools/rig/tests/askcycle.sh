#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --fixture fixtures/ask.jsonl); trap '$R stop $n' EXIT
$R wait $n '❯'
first=$($R snap $n | head -1 | grep -oE '@[a-z0-9-]+')
$R type $n '/new second tab'
$R send $n Enter
$R wait $n 'esc dismiss'
second=$($R tab $n)
$R send $n S-Up
$R idle $n
[ "$($R tab $n)" = "$first" ]
$R send $n S-Down
$R idle $n
[ "$($R tab $n)" = "$second" ]
$R wait $n 'esc dismiss'
