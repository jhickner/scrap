#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./muxrig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n '/new second tab'
$R send $n Enter
$R wait $n 'session 2/2'
$R wait $n 'echo: second tab'
$R send $n BTab
$R wait $n '^\[ untitled \]  untitled'
$R send $n BTab
$R wait $n 'echo: second tab'
