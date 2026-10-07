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
$R say $b 'run: sleep 6' 'thinking'
$R say $a 'run: scrap send @b hi; sleep 6' 'thinking'
$R wait $b 'from @a \(answered\): hi'
$R wait $a 'from @b: fork: hi'
$R wait $b 'echo: \[from @a; a fork of this session already replied: "fork: hi"\] hi'
$R wait $a 'echo: \[from @b\] fork: hi'
$R idle $b
! $R snap $b | grep -q 'fork: fork'
