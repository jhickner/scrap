#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "sessionsnet: skipped, no tailscale address"; exit 0; }
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\ntailscale = %s\n' $((20000 + RANDOM % 20000)) "$(tailscale ip -4 | head -1)" "$(pwd)/fake-tailscale" > "$share/net"
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --share "$share" --net)
c=$($R start --fake --share "$share" --net)
trap 'for n in $a $b $c; do $R stop $n 2>/dev/null || true; done; rm -rf "$share"' EXIT
fail() { echo "sessionsnet: $*" >&2; exit 1; }
for n in $a $b $c; do
    $R wait $n '❯'
    $R say $n hello 'echo: hello'
done
$R say $b '/name bee' 'now @bee'
$R say $c '/name cee' 'now @cee'
to() { for _ in $(seq 20); do $R snap $a | grep -q "→.*$1" && return 0; $R send $a Down; $R idle $a 150; done; fail "no row $1: $($R snap $a)"; }

$R say $a '/sessions' 'sessions'
$R snap $a | grep -q 'mirror' && fail "remote shown before *"
$R send $a '*'
$R wait $a 'blackhole · checking' 3
$R wait $a '⌂ '
to 'claude fake @bee  *just now'
$R wait $a 'blackhole · no answer' 8
$R snap $a | grep -q '→.*claude fake @bee  *just now' || fail "cursor moved on refresh: $($R snap $a)"

$R send $a r
$R idle $a 300
$R type $a 'beehive'
$R send $a Enter
$R wait $a '→ *beehive *claude fake @bee  *just now' 5

$R send $a Enter
$R wait $a '\[ mirror:@bee \]' 5
$R say $a 'over the mirror' 'echo: over the mirror'
$R wait $b 'echo: over the mirror'

$R say $a '/sessions' 'sessions'
$R wait $a '^  mirror$' 5
$R snap $a | grep -q 'beehive *claude fake @bee  *just now' && fail "attached session listed twice: $($R snap $a)"
to 'claude fake @cee  *just now'
$R send $a x
$R wait $c 'Pane is dead' 5
$R idle $a 300
$R send $a Escape
$R idle $a 300

$R say $a '/sessions' 'sessions'
$R send $a '*'
$R idle $a 300
$R snap $a | grep -q '^  mirror' && fail "remote shown after second *"
to 'fake @bee'
$R send $a s
$R idle $a 300
$R type $a 'hi bee'
$R send $a Enter
$R wait $b 'hi bee'
echo "sessionsnet: ok"
