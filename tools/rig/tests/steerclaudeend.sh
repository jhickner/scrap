#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n 'slow: 4 hi'
$R send $n Enter
$R wait $n '⠋|⠙|⠹|⠸|⠼|⠴|⠦|⠧|⠇|⠏'
$R say $n 'and more' 'steer · sent.* · and more'
$R wait $n 'echo: and more' 15
$R idle $n
screen=$($R snap $n)
grep -q 'slowdone: hi' <<<"$screen" || { echo "steerclaudeend: first answer missing" >&2; echo "$screen" >&2; exit 1; }
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 1 ] || { echo "steerclaudeend: want the steer in the same turn" >&2; echo "$screen" >&2; exit 1; }
echo "steerclaudeend: ok"
