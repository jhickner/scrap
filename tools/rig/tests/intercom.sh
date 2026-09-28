#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=$($R start --fake --share "$share")
trap '$R stop $a; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
    $R type $n hello
    $R send $n Enter
    $R wait $n 'echo: hello'
done
$R type $a '/name a'
$R send $a Enter
$R wait $a 'this session is now @a'
$R type $b '/name b'
$R send $b Enter
$R wait $b 'this session is now @b'
$R type $a 'run: scrap send @b hi'
$R send $a Enter
$R wait $a 'ran: sent to @b'
$R wait $b 'from @a: hi'
$R wait $b 'echo: \[from @a\] hi'
$R type $a 'run: scrap send @nobody hi'
$R send $a Enter
$R wait $a 'ran: scrap: no session matches @nobody'
$R stop $b
$R type $a 'run: scrap send @b hi'
$R send $a Enter
$R wait $a 'ran: scrap: @b is not live; resume it with .?scrap open @b'
$R type $a 'run: scrap open @b'
$R send $a Enter
$R wait $a 'ran: opened @b in a new tab'
$R wait $a '│ [^@]*@b *$'
[ "$($R tab $a)" = @a ]
