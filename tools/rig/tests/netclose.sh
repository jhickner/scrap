#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
$R type $b '/new second tab'
$R send $b Enter
$R wait $b 'session 2/2'
$R say $b '/name doomed' 'this session is now @doomed'
$R type $b 'run: sleep 30'
$R send $b Enter
$R wait $b 'sleep 30'
out=$(env -u SCRAP_PID $R cli $a close "$me:@doomed" 2>&1) || fail "close over the net: $out"
until_ok sh -c "! $R cli $a ls --live | grep -q '^@doomed '" || fail "@doomed is still live"
out=$(env -u SCRAP_PID $R cli $a close "$me:@b" 2>&1) || fail "close the last tab over the net: $out"
until_ok sh -c "! $R cli $a ls --live | grep -q '^@b '" || fail "@b is still live"
out=$(env -u SCRAP_PID $R cli $a close @a 2>&1) || fail "local close from outside a window: $out"
until_ok sh -c "! $R cli $a ls --live | grep -q '^@a '" || fail "@a is still live"
echo "netclose: ok"
