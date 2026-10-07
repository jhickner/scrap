#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'ask: touch a.txt' 'y allow  n deny'
$R wait $n 'Bash · run a command'
$R wait $n '│ \? @'
$R type $n y
$R wait $n 'allowed: touch a.txt'
$R say $n 'ask: touch b.txt' '  touch b.txt'
$R type $n n
$R wait $n 'denied: touch b.txt'
