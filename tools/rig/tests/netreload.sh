#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
pid=$($R broker $a)
kill -URG "$pid"
until_ok $R cli $a send "$me:@b" after reload
$R wait $b 'after reload'
[ "$($R broker $a)" = "$pid" ] || fail "reload changed the broker pid"
echo "netreload: ok"
