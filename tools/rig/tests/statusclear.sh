#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
scrap=$(cat "$($R dir $n)/scrap")
$R wait $n '❯'
$R type $n "run: $scrap status busytest; sleep 3"
$R send $n Enter
for _ in $(seq 20); do
    $R cli $n ls --live | grep -q busytest && break
    sleep 0.2
done
if ! $R cli $n ls --live | grep -q busytest; then
    $R cli $n ls --live >&2
    echo "statusclear: status was not set during the turn" >&2
    exit 1
fi
$R wait $n 'ran:'
$R idle $n 1000
if $R cli $n ls --live | grep -q busytest; then
    $R cli $n ls --live >&2
    echo "statusclear: status survived the end of the turn" >&2
    exit 1
fi
echo "statusclear: ok"
