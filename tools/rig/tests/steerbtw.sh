#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4' '\[bash\] sleep 4'
$R say $n '/btw add a todo: renew passport' '/btw add a todo'
$R snap $n | grep -q 'steer ·' && { echo "steerbtw: /btw was steered" >&2; $R snap $n >&2; exit 1; }
$R say $n 'add a todo: buy milk' 'steer · .* · add a todo: buy milk'
$R wait $n 'steered: add a todo: buy milk' 15
$R idle $n
$R snap $n | grep -q 'echo: /btw' && { echo "steerbtw: /btw reached the turn" >&2; $R snap $n >&2; exit 1; }
echo "steerbtw: ok"
