#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
w=/tmp/scraprig/$n/work
hud() { $R say $n '!true'; $R idle $n; $R send $n Enter; }
$R wait $n '❯'
git -C $w init -q
git -C $w config user.email t@t
git -C $w config user.name t
git -C $w config commit.gpgsign false
echo one > $w/tracked.txt
git -C $w add tracked.txt
git -C $w commit -qm init
hud
$R wait $n 'work on .+ \([0-9a-f]+\)$'
! $R snap $n | grep -Eq 'work on .*(\[|\+[0-9])'
echo two >> $w/tracked.txt
hud
$R wait $n 'work on .+ \([0-9a-f]+\) \+1 \[!\]$'
touch $w/new.txt
hud
$R wait $n 'work on .+ \([0-9a-f]+\) \+1 \[!\?\]$'
