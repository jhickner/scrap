#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
fail() { echo "new: $*" >&2; exit 1; }
a=$($R start --fake)
trap '$R stop $a' EXIT
$R wait $a '❯'
$R say $a 'run: scrap new first task' 'ran: started @'
$R send $a S-Down
$R wait $a 'echo: first task'
$R say $a 'run: scrap new -C /tmp second task' 'ran: started @'
$R send $a S-Down
$R wait $a 'echo: second task'
out=$(env -u SCRAP_PID $R cli $a new hi 2>&1 || true)
echo "$out" | grep -q 'runs inside a scrap session or takes --on' || fail "outside a session: $out"
out=$(env -u SCRAP_PID $R cli $a new -x 2>&1 || true)
echo "$out" | grep -q 'usage: scrap new' || fail "bad flag: $out"
echo "new: ok"
