#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4' 'thinking'
$R say $n 'add a todo: renew passport' '/btw add a todo'
$R idle $n
! $R snap $n | grep -q 'echo: add a todo'
