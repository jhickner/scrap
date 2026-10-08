#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "memoryidless: skipped, no tailscale address"; exit 0; }
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\ntailscale = %s\nmachines = mirror\n' $((20000 + RANDOM % 20000)) "$(tailscale ip -4 | head -1)" "$(pwd)/fake-tailscale" > "$share/net"
a=$($R start -s 110x40 --fake --share "$share" --net)
b=$($R start -s 110x40 --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
$R wait $a '❯'
$R wait $b '❯'
$R say $b '/memory on' 'memory on'
$R say $b '/name mem' 'now @mem'
$R type $b 'memrun: scrap status remembering things; sleep 25'
$R send $b Enter
for _ in $(seq 40); do $R cli $a ls | grep -q '@mem .*remembering things' && break; sleep 0.25; done
$R cli $a ls | grep -q '@mem .*working.*remembering things' ||
    { echo "memoryidless: scrap status did not reach the live list" >&2; $R cli $a ls >&2; exit 1; }
$R say $a '/sessions' 'sessions'
$R wait $a '⠋|⠙|⠹|⠸|⠼|⠴|⠦|⠧|⠇|⠏' 5
$R send $a '*'
$R wait $a '^  mirror' 8
$R wait $a '^    [^ ] remembering things +claude fake @mem' 10
[ "$($R snap $a | grep -cE '^    [^ ] remembering things +claude fake @mem')" = 2 ] ||
    { echo "memoryidless: want the session under this machine and under mirror" >&2; $R snap $a >&2; exit 1; }
echo "memoryidless: ok"
