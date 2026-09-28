#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
ip=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [ -z "$ip" ]; then echo "remotetab: skipped, no tailscale address"; exit 0; fi
me=$(tailscale status --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["Self"]["DNSName"].split(".")[0])')
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\n' "$((20000 + RANDOM % 20000))" "$ip" > "$share/net"
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
    $R type $n hello
    $R send $n Enter
    $R wait $n 'echo: hello'
done
$R type $b '/name bee'
$R send $b Enter
$R wait $b 'now @bee'

$R type $a "/attach $me:@bee"
$R send $a Enter
$R wait $a "\[ $me:@bee \]"
$R wait $a '▌ hello'
$R type $a 'from a'
$R send $a Enter
$R wait $b 'echo: from a'
$R wait $a 'echo: from a'
$R type $b 'typed on b'
$R send $b Enter
$R wait $a '▌ typed on b'
$R wait $a 'echo: typed on b'
$R type $a '/attach @nobody'
$R send $a Enter
$R wait $a 'no live session matches @nobody'
echo "remotetab: ok"
