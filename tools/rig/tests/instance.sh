#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=""
trap '[ -z "$a" ] || $R stop $a; [ -z "$b" ] || $R stop $b; rm -rf "$share"' EXIT
$R wait $a '❯'
$R say $a 'first tab' 'echo: first tab'
$R say $a '/new second tab' 'echo: second tab'
names=$($R snap $a | head -4 | grep -Eo '@[a-z]+(-[0-9]+)?')
$R say $a '/save work' 'saved 2 tabs as work'
$R stop $a; a=""
b=$($R start --fake --share "$share" -- --instance work)
for name in $names; do $R wait $b "$name"; done
[ "$($R tab $b)" = "$(head -1 <<<"$names")" ]
