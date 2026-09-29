#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
kill -9 "$($R broker $a)"
until_ok bash -c '! '"$R"' broker '"$a"
out=$($R cli $a send "$me:@b" nobody home 2>&1 || true)
echo "$out" | grep -q 'no scrap running' || fail "send without a broker: $out"
c=$($R start --fake --share "$share" --net)
extra=$c
$R wait $c '❯'
until_ok $R cli $a send "$me:@b" after restart
$R wait $b 'after restart'
echo "netrestart: ok"
