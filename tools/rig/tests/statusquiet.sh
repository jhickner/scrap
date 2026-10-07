#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -- --name rig)
trap '$R stop $n' EXIT
$R wait $n '❯'
echo "status_seconds=2" >> "/tmp/scraprig/$n/state/config/settings"
$R say $n 'run: sleep 12' 'thinking'
$R wait $n 'ran:' 20
[ "$($R snap $n | grep -c '✓ status update')" -eq 1 ]
