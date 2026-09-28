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

$R say $a "run: scrap send $me:@b over the net" "ran: sent to $me:@b"
$R wait $b "from $me:@a: over the net"
$R say $a "run: scrap send $me:@nobody hi" "ran: scrap: $me: no live session matches @nobody"
out=$($R cli $a read "$me:@b" -n 1 2>&1 || true)
echo "$out" | grep -q "$me: no transcript for @b" || fail "read over the net: $out"

read -r ip port < <($R net $a)
holder=$(lsof -nP -iTCP@"$ip":"$port" -sTCP:LISTEN -Fp 2>/dev/null | sed -n 's/^p//p' | head -1)
[ -n "$holder" ] || fail "no process holds $ip:$port"
if grep -l "\"pid\":$holder," "$share"/live/*.json 2>/dev/null | xargs grep -q '"name":"a"'; then
    gone=$a; left=$b; keep=b
else
    gone=$b; left=$a; keep=a
fi
$R stop $gone
until_ok $R cli $left send "$me:@$keep" after failover
$R wait $left "after failover"
echo "net: ok"
