#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
scrap="$(pwd)/../../scrap"
ip=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [ -z "$ip" ]; then echo "net: skipped, no tailscale address"; exit 0; fi
me=$(tailscale status --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["Self"]["DNSName"].split(".")[0])')
port=$((20000 + RANDOM % 20000))
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\n' "$port" "$ip" > "$share/net"
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "net: $*" >&2; exit 1; }
cfg() { echo "/tmp/scraprig/$1/state/config"; }
for n in $a $b; do
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

until_ok() { for _ in $(seq 60); do "$@" >/dev/null 2>&1 && return 0; sleep 0.25; done; "$@"; }
until_ok env SCRAP_CONFIG_DIR="$(cfg $a)" "$scrap" send "$me:@b" warmup
$R wait $b "from $me:@a: warmup" 2>/dev/null || $R wait $b 'warmup'

$R type $a "run: scrap send $me:@b over the net"
$R send $a Enter
$R wait $a "ran: sent to $me:@b"
$R wait $b "from $me:@a: over the net"
$R type $a "run: scrap send $me:@nobody hi"
$R send $a Enter
$R wait $a "ran: scrap: $me: no live session matches @nobody"
out=$(SCRAP_CONFIG_DIR="$(cfg $a)" "$scrap" read "$me:@b" -n 1 2>&1 || true)
echo "$out" | grep -q "$me: no transcript for @b" || fail "read over the net: $out"

holder=$(lsof -nP -iTCP@"$ip":"$port" -sTCP:LISTEN -Fp 2>/dev/null | sed -n 's/^p//p' | head -1)
[ -n "$holder" ] || fail "no process holds $ip:$port"
if grep -q "\"pid\":$holder," "$share"/live/*.json && grep -l "\"pid\":$holder," "$share"/live/*.json | xargs grep -q '"name":"a"'; then
    gone=$a; left=$b; keep=b
else
    gone=$b; left=$a; keep=a
fi
$R stop $gone
until_ok env SCRAP_CONFIG_DIR="$(cfg $left)" "$scrap" send "$me:@$keep" after failover
$R wait $left "after failover"
echo "net: ok"
