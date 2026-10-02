#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'ask: touch a.txt' 'allow Bash: touch a.txt\? y/n'
$R type $n y
$R wait $n 'allowed: touch a.txt'
$R say $n 'ask: touch b.txt' 'allow Bash: touch b.txt\? y/n'
$R type $n n
$R wait $n 'denied: touch b.txt'
