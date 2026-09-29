#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -- --name rig); trap '$R stop $n' EXIT
$R wait $n '❯'
$R idle $n
$R expect $n golden/startup.txt "$@"
