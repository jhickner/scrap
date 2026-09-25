#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./muxrig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n /
$R wait $n '/new  start a fresh conversation'
$R idle $n
$R expect $n golden/slash.txt "$@"
