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

$R say $b 'run: sleep 3' 'thinking'
$R say $a 'run: scrap send @b slowfork hi' 'ran: sent to @b'
$R wait $b 'echo: \[from @a\] slowfork hi'
sleep 8
! $R snap $a | grep -q 'from @b'
! $R snap $b | grep -q 'answered'
