#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "net: skipped, no tailscale address"; exit 0; }
me=$($R machine)
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "net: $*" >&2; exit 1; }
for n in $a $b; do
    $R wait $n '❯'
    $R say $n hello 'echo: hello'
done
$R say $a '/name a' 'this session is now @a'
$R say $b '/name b' 'this session is now @b'

until_ok() { for _ in $(seq 60); do "$@" >/dev/null 2>&1 && return 0; sleep 0.25; done; "$@"; }
until_ok $R cli $a send "$me:@b" warmup
$R wait $b 'warmup'

read -r ip port < <($R net $a)
broker() { lsof -nP -iTCP@"$ip":"$port" -sTCP:LISTEN -Fp 2>/dev/null | sed -n 's/^p//p' | head -1; }
pid=$(broker)
[ -n "$pid" ] || fail "no broker on $ip:$port"
grep -q "\"pid\":$pid," "$share"/live/*.json && fail "the broker is a session process"

for to in a b; do
    from=$([ $to = a ] && echo $b || echo $a)
    $R say $from "run: scrap send $me:@$to over the net" "ran: sent to $me:@$to"
    $R wait $([ $to = a ] && echo $a || echo $b) "from $me:@[ab]: over the net"
done
$R say $a "run: scrap send $me:@nobody hi" "ran: scrap: $me: no live session matches @nobody"
out=$($R cli $a read "$me:@b" -n 1 2>&1 || true)
echo "$out" | grep -q "$me: no transcript for @b" || fail "read over the net: $out"

kill -URG "$pid"
until_ok $R cli $a send "$me:@b" after reload
$R wait $b 'after reload'
[ "$(broker)" = "$pid" ] || fail "reload changed the broker pid"

(sleep 8) | $R cli $a attach "$me:@b" > "$share/stream" 2>&1 &
until_ok grep -q '"history":' "$share/stream"
kill -9 "$pid"
$R say $b 'after the broker died' 'echo: after the broker died'
until_ok grep -q 'after the broker died' "$share/stream" || fail "attached stream died with the broker: $(cat "$share/stream")"
out=$($R cli $a send "$me:@b" nobody home 2>&1 || true); echo "$out" | grep -q 'no scrap running' || fail "send without a broker: $out"
c=$($R start --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; $R stop $c 2>/dev/null || true; rm -rf "$share"' EXIT
$R wait $c '❯'
until_ok $R cli $a send "$me:@b" after restart
$R wait $b 'after restart'
echo "net: ok"
