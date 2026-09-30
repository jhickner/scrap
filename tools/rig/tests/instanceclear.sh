#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=""
trap '[ -z "$a" ] || $R stop $a; [ -z "$b" ] || $R stop $b; rm -rf "$share"' EXIT
$R wait $a '❯'
$R say $a 'topic one' 'echo: topic one'
$R say $a '/save work' 'saved 1 tab as work'
$R say $a '/clear' '❯'
$R say $a 'topic two' 'echo: topic two'
latest=$(grep -o '"id":"[^"]*"' "$share"/live/*.json)
$R stop $a; a=""
b=$($R start --fake --share "$share" -- --instance work)
$R wait $b '❯'
[ "$(grep -o '"id":"[^"]*"' "$share"/live/*.json)" = "$latest" ]
