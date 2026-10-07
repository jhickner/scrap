#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fixture fixtures/bgmove.jsonl); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'wait' 'bash +sleep 99'
$R send $n Tab
$R wait $n '▸ . bash +sleep 99'
if $R snap $n -e | grep -q $'\e\\[7m'; then
    $R snap $n >&2; echo "taskfocus: the prompt cursor is drawn while focus is on a task" >&2; exit 1
fi
$R send $n Enter
$R wait $n '^ {6}sleep 99'
$R send $n Enter
sleep 1
if $R snap $n | grep -qE '^ {6}sleep 99'; then
    $R snap $n >&2; echo "taskfocus: enter did not collapse the command" >&2; exit 1
fi
$R type $n x
$R wait $n '× bash|✓ bash'
sleep 3
if $R snap $n | grep -q 'bash +sleep 99'; then
    $R snap $n >&2; echo "taskfocus: x did not stop the task" >&2; exit 1
fi
$R type $n 'still typing'
$R wait $n '❯ still typing'
echo "taskfocus: ok"
