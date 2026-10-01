#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 6' 'thinking'
$R say $n 'then commit it' '▌ then commit it'
$R type $n 'no, use the other folder'
$R send $n Enter
$R wait $n 'echo: no, use the other folder'
$R wait $n 'queued: then commit it'
$R send $n C-x
$R wait $n '❯ then commit it'
