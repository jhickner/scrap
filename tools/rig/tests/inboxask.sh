#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share" -- --name a)
b=$($R start --fake --share "$share" -- --name b)
trap '$R stop $a; $R stop $b; rm -rf "$share"' EXIT
$R wait $a '❯'
$R wait $b '❯'

$R say $b askme 'esc dismiss'
$R send $b Down
$R send $b Down
$R idle $b
$R type $b green
$R wait $b '✎ green'
$R say $a 'run: scrap send @b skipme ping' 'ran: sent to @b'
sleep 2
$R wait $b '✎ green'
$R wait $b 'esc dismiss'
! $R snap $b | grep -q 'echo: \[from @a\]'
$R send $b Escape
$R wait $b 'echo: \[from @a\] skipme ping'
