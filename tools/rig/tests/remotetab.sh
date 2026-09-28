#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "remotetab: skipped, no tailscale address"; exit 0; }
me=$($R machine)
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --share "$share" --net)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
    $R say $n hello 'echo: hello'
done
$R say $b '/name bee' 'now @bee'

$R say $a "/attach $me:@bee" "│ [^@]*$me:@bee *\$"
$R wait $a '▌ hello'
$R say $a 'from a' 'echo: from a'
$R wait $b 'echo: from a'
$R say $b 'typed on b' 'echo: typed on b'
$R wait $a '▌ typed on b'
$R wait $a 'echo: typed on b'
$R say $a '/restart' 'restarted 1x' 15
$R wait $a "│ [^@]*$me:@bee *\$"
$R say $a 'after restart'
$R wait $b 'echo: after restart'
$R say $a "/attach $me:@bee"
$R idle $a
$R snap $a | grep -q 'no live session' && { $R snap $a; echo "remotetab: re-attach failed"; exit 1; }
[ "$($R tab $a)" = "$me:@bee" ]
[ "$($R snap $a | head -4 | grep -c '│ [^@]*@[a-z0-9_-]* *$')" = 2 ]
$R say $a '/attach @nobody' 'no live session matches @nobody'
echo "remotetab: ok"
