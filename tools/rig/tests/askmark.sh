#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
first=$($R tab $n)
$R type $n "run: sleep 3; printf 'Pick.\n\n@ask\n1. Which color?\n- red\n- blue'"
$R send $n Enter
$R send $n C-n
$R idle $n
[ "$($R tab $n)" != "$first" ]
$R wait $n "│ \? $first" 15
$R send $n S-Up
$R wait $n 'esc dismiss'
$R wait $n "│ \? $first"
$R send $n Escape
$R idle $n
! $R snap $n | grep -q "│ ? $first"
