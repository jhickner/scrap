#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
first=$($R snap $n | head -1 | grep -oE '@[a-z0-9-]+')
$R type $n '/new second tab'
$R send $n Enter
$R wait $n 'session 2/2'
$R wait $n 'echo: second tab'
$R send $n BTab
$R idle $n
$R snap $n | head -1 | grep -q "│ [^@]*$first *\$"
$R send $n BTab
$R wait $n 'echo: second tab'
