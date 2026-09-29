#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
pid=$($R broker $a)
grep -q "\"pid\":$pid," "$share"/live/*.json && fail "the broker is a session process"
for to in a b; do
    from=$([ $to = a ] && echo $b || echo $a)
    $R say $from "run: scrap send $me:@$to over the net" "ran: sent to $me:@$to"
    $R wait $([ $to = a ] && echo $a || echo $b) "from $me:@[ab]: over the net"
done
$R say $a "run: scrap send $me:@nobody hi" "ran: scrap: $me: no live session matches @nobody"
out=$($R cli $a read "$me:@b" -n 1 2>&1 || true)
echo "$out" | grep -q "$me: no transcript for @b" || fail "read over the net: $out"
echo "net: ok"
