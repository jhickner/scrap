#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
trap '$R stop $a; $R stop $b; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
    $R say $n hello 'echo: hello'
done
$R say $a '/name a' 'this session is now @a'
$R say $b '/name b' 'this session is now @b'
$R say $b 'run: sleep 60' 'thinking'
$R say $a 'run: scrap send --interrupt @b stop' 'ran: sent to @b'
$R wait $b 'interrupted: sleep 60'
$R wait $b 'echo: \[from @a\] stop'
