#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "netpick: skipped, no tailscale address"; exit 0; }
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; rm -rf "$share"' EXIT
echo "tailscale = $(pwd)/fake-tailscale" >> "$share/net"
fail() { echo "netpick: $*" >&2; exit 1; }
$R wait $a '❯'
$R say $a hi 'echo: hi'
$R say $a '/net' 'blackhole · checking' 2
$R send $a Down
$R idle $a 300
$R wait $a 'blackhole · no answer' 8
$R snap $a | grep -q '→ *+ new session' || fail "cursor moved on refresh: $($R snap $a)"
$R send $a Escape
echo "netpick: ok"
