#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "steernet: skipped, no tailscale address"; exit 0; }
me=mirror
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\ntailscale = %s\n' $((20000 + RANDOM % 20000)) "$(tailscale ip -4 | head -1)" "$(pwd)/fake-tailscale" > "$share/net"
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --openai --share "$share" --net -s 120x40 -- -b core -m fake/echo)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "steernet: $*" >&2; $R snap $b >&2; exit 1; }
until_ok() { for _ in $(seq 60); do "$@" >/dev/null 2>&1 && return 0; sleep 0.25; done; "$@"; }
$R wait $a '❯'
$R wait $b '❯'
$R say $a hello 'echo: hello'
$R say $a '/name a' 'this session is now @a'
$R say $b '/name bee' 'now @bee'
until_ok $R cli $a send "$me:@bee" warmup
$R wait $b 'echo: \[from [^]]*\] warmup' 15
$R idle $b

$R say $b 'run: sleep 4; echo done' '\[bash\] sleep 4'
out=$($R cli $a send --from a --steer "$me:@bee" go left 2>&1)
[ "$out" = "sent to $me:@bee (steered)" ] || fail "over the net: $out"
$R wait $b 'steered: \[from [^]]*@a\] go left' 15
$R idle $b

$R stream $a s "$me:@bee"
$R stream-wait $a s '"history":'
$R say $b 'run: sleep 4; echo again' '\[bash\] sleep 4; echo again'
$R stream-put $a s '!steer from the tab'
$R wait $b 'steered: from the tab' 15
$R stream-wait $a s '"kind":"user","text":"from the tab"' 15
$R idle $b
[ "$($R snap $b | grep -cE '^[0-9]+s( · |$)')" = 3 ] || fail "want the steers inside their turns"
echo "steernet: ok"
