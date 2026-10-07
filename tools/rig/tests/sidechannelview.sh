#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 40x30); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 8' 'thinking'
$R say $n '/btw why does this pending question need more than one row of a narrow screen to be shown fully ENDMARK' '▌ +ENDMARK'
$R wait $n '▌ ⠋? */btw why does this pending'
! $R snap $n | grep -q '…'
