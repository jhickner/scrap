#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 120x40); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 20; echo done' '\[bash\] sleep 20'
$R say $n 'do this instead' 'steer · sent.* · do this instead'
$R send $n Escape
$R wait $n 'echo: do this instead' 15
$R idle $n
screen=$($R snap $n)
grep -q '^▌ do this instead$' <<<"$screen" || { echo "steerclaudeesc: no user block" >&2; echo "$screen" >&2; exit 1; }
grep -q 'interrupted: sleep 20' <<<"$screen" || { echo "steerclaudeesc: tool was not interrupted" >&2; echo "$screen" >&2; exit 1; }
$R send $n Enter
$R wait $n '6 in / 8 out' 5 || { echo "steerclaudeesc: tokens of the follow-on turn replaced the first part" >&2; $R snap $n >&2; exit 1; }
echo "steerclaudeesc: ok"
