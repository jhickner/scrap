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
$R send $n BTab
$R wait $n '^\[ @[a-z]+(-[0-9]+)? \]  @[a-z]+(-[0-9]+)?'
$R send $n BTab
$R wait $n 'echo: second tab'
