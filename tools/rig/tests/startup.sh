#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./muxrig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R idle $n
$R expect $n golden/startup.txt "$@"
