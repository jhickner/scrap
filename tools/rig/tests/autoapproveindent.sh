#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n '/autoapprove on' 'auto-approve on'
$R type $n 'permrun: echo one; '
$R send $n M-Enter
$R type $n 'echo two'
$R send $n Enter
$R wait $n 'ran: one'
$R idle $n
dim=$'\e\\[38;2;107;115;148m'
$R snap $n -e | grep -qE "^    $dim\[bash\] auto-approved: echo one; echo two"
$R wait $n '^    one$'
