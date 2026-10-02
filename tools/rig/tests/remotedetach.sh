#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
done
$R say $b '/name bee' 'now @bee'
$R say $a '/attach @bee' '@bee'
$R idle $a
$R type $a 'run: sleep 3; echo finished'
$R send $a Enter
$R wait $b 'sleep 3'
$R send $a C-d
$R wait $b 'ran: finished' 10
$R idle $b
$R snap $b | grep -q '^ *interrupted' && { $R snap $b; echo "remotedetach: turn interrupted"; exit 1; }
echo "remotedetach: ok"
