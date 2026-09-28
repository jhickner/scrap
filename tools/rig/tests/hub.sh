#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
scrap="$(pwd)/../../scrap"
ip=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [ -z "$ip" ]; then echo "hub: skipped, no tailscale address"; exit 0; fi
me=$(tailscale status --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["Self"]["DNSName"].split(".")[0])')
port=$((20000 + RANDOM % 20000))
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\n' "$port" "$ip" > "$share/hub"
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
SCRAP_CONFIG_DIR="$share" "$scrap" hub > "$share/hub.log" 2>&1 &
hub=$!
trap 'kill $hub 2>/dev/null; $R stop $a 2>/dev/null; $R stop $b 2>/dev/null; rm -rf "$share"' EXIT
for n in $a $b; do
    cp "$share/hub" "/tmp/scraprig/$n/state/config/hub"
    $R wait $n '❯'
    $R type $n hello
    $R send $n Enter
    $R wait $n 'echo: hello'
done
$R type $a '/name a'
$R send $a Enter
$R wait $a 'this session is now @a'
$R type $b '/name b'
$R send $b Enter
$R wait $b 'this session is now @b'
for _ in $(seq 50); do grep -q "on $ip:$port" "$share/hub.log" && break; sleep 0.1; done
grep -q "on $ip:$port" "$share/hub.log" || { echo "hub: did not start: $(cat "$share/hub.log")"; exit 1; }
out=$(SCRAP_CONFIG_DIR="/tmp/scraprig/$a/state/config" "$scrap" ls --net)
echo "$out" | grep -Eq "^$me\$" || { echo "hub: ls --net lacks $me: $out"; exit 1; }
echo "$out" | grep -Eq '^  @b +live ' || { echo "hub: ls --net lacks @b: $out"; exit 1; }
$R type $a "run: scrap send $me:@b over the net"
$R send $a Enter
$R wait $a "ran: sent to $me:@b"
$R wait $b "from $me:@a: over the net"
$R wait $b "echo: \[from $me:@a\] over the net"
$R type $a "run: scrap send $me:@nobody hi"
$R send $a Enter
$R wait $a "ran: scrap: $me: no session matches @nobody"
out=$(SCRAP_CONFIG_DIR="/tmp/scraprig/$a/state/config" "$scrap" read "$me:@b" -n 1 2>&1 || true)
echo "$out" | grep -q "$me: no transcript for @b" || { echo "hub: read over the net: $out"; exit 1; }
echo "hub: ok"
