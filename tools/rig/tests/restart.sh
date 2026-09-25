#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./muxrig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n '/new second tab'
$R send $n Enter
$R wait $n 'session 2/2'
$R type $n /restart
$R send $n Enter
$R wait $n 'restarted 1x'
$R wait $n 'untitled.*untitled'
[ -z "$(ls "$($R dir $n)/state/tmp")" ] || { echo "restart left files in state/tmp" >&2; exit 1; }
