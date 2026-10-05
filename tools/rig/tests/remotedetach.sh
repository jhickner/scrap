#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "remotedetach: skipped, no tailscale address"; exit 0; }
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\ntailscale = %s\n' $((20000 + RANDOM % 20000)) "$(tailscale ip -4 | head -1)" "$(pwd)/fake-tailscale" > "$share/net"
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
done
$R say $b '/name bee' 'now @bee'
$R say $a '/attach mirror:@bee' '@bee'
$R idle $a
$R type $a 'run: sleep 3; echo finished'
$R send $a Enter
$R wait $b 'sleep 3'
$R send $a C-d
$R wait $b 'ran: finished' 10
$R idle $b
$R snap $b | grep -q '^ *interrupted' && { $R snap $b; echo "remotedetach: turn interrupted"; exit 1; }
echo "remotedetach: ok"
