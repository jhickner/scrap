#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
out=$(env -u SCRAP_PID $R cli $a new --on "$me" -C /tmp remote task)
echo "$out" | grep -q "^started $me:@" || fail "spawn over the net: $out"
name=${out#started $me:}
$R cli $a ls --live | grep -q "^$name " || fail "$name is not live"
echo "netnew: ok"
