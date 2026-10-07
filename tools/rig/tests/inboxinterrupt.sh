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

$R say $b 'run: sleep 30' 'thinking'
$R say $a 'run: scrap send @b skipme one; scrap send --interrupt @b skipme two' 'ran: sent to @b'
$R wait $b 'echo: \[from @a\] skipme two'
$R wait $b '^ *\[from @a\] skipme one'
