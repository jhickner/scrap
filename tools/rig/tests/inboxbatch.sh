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

$R say $b 'run: sleep 5' 'thinking'
$R say $a 'run: scrap send @b skipme one; scrap send @b skipme two' 'ran: sent to @b'
$R type $b 'typed line'
$R send $b M-Enter
$R wait $b '▌ typed line'
$R wait $b 'echo: \[from @a\] skipme one'
$R wait $b '^ *\[from @a\] skipme two'
$R wait $b 'echo: typed line'
$R idle $b
! $R snap $b | grep -q 'echo: \[from @a\] skipme two'
! $R snap $a | grep -q 'from @b'
