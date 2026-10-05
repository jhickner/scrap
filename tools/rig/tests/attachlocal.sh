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
    $R say $n hello 'echo: hello'
done
$R say $b '/name bee' 'now @bee'
$R say $a '/attach @bee'
$R idle $a
[ "$($R tab $a)" = "@bee" ] || { $R snap $a; echo "attachlocal: @bee is not a tab in a"; exit 1; }
[ "$($R tab $b)" != "@bee" ] || { $R snap $b; echo "attachlocal: @bee still in b"; exit 1; }
echo "attachlocal: ok"
