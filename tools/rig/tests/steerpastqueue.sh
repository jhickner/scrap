#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 6' 'thinking'
$R type $n 'then commit it'
$R send $n M-Enter
$R wait $n '▌ then commit it'
$R type $n 'no, use the other folder'
$R send $n Enter
$R wait $n 'steered: no, use the other folder' 15
$R wait $n 'echo: then commit it' 15
