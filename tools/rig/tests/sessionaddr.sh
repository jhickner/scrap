#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
d=/tmp/scraprig/$n/state/config/addr
$R wait $n '❯'
$R say $n '/name doomed' 'this session is now @doomed'
$R say $n 'run: cat $MUX_SESSION_FILE' 'ran: fake-'
f=$(ls $d/*)
[ "$(cat $f)" = "$(sed -n 's/^ran: //p' <<<"$($R snap $n)" | head -1)" ]
id=$(cat $f)
$R say $n '/clear' '❯'
$R say $n 'run: cat $MUX_SESSION_FILE' 'ran: fake-[0-9a-f]{8}$'
[ "$(cat $f)" != "$id" ]
[ "$(cat $f)" = "$(sed -n 's/^ran: //p' <<<"$($R snap $n)" | tail -1)" ]
[ "$(wc -l < $f)" = 1 ]
$R say $n '/new' 'start'
$R send $n Enter
$R wait $n '2/2'
[ "$(ls $d | wc -l)" = 2 ]
env -u SCRAP_PID $R cli $n close @doomed >/dev/null
until [ ! -e $f ]; do sleep 0.1; done
[ "$(ls $d | wc -l)" = 1 ]
