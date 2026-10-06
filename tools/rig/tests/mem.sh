#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -s 110x30); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n '/mem'
$R send $n Enter
$R wait $n 'mem · personal · 2 open'
$R wait $n '→ ○ buy milk'
$R type $n 'x'
$R wait $n 'mem · personal · 1 open'
$R type $n 'n'
$R type $n 'call bob'
$R send $n Enter
$R wait $n '→ ○ call bob'
$R type $n 'n'
$R type $n 'first line'
$R send $n C-j
$R type $n "$(printf 'word%.0s ' {1..30})tail"
$R wait $n 'word tail'
$R send $n Left
$R type $n 'X'
$R wait $n 'taiXl'
$R send $n Enter
$R wait $n '→ ○ first line'
$R send $n Enter
$R wait $n 'word word'
$R type $n 'h'
$R wait $n '→ ○ first line'
$R type $n '#'
$R type $n 'sc'
$R send $n Tab
$R send $n Enter
$R wait $n '# scrap'
$R wait $n '→ ○ Rig plan'
$R send $n Enter
$R wait $n 'write the rig test'
$R wait $n '• notes.txt'
$R type $n $'\e[<0;35;12M'
$R wait $n '2/2 · blue.png'
$R send $n Escape
$R wait $n 'write the rig test'
$R type $n 'i'
$R wait $n '1/2 · red.png'
$R send $n Escape
$R wait $n 'write the rig test'
$R type $n '*'
$R wait $n 'rev 2  todo  scrap  starred'
$R type $n 'd'
$R wait $n 'delete "Rig plan"\? y/n'
$R type $n 'n'
$R send $n Escape
$R wait $n '→ ○ Rig plan'
$R type $n '3'
$R wait $n '\[starred\]'
$R wait $n '★ old idea'
$R type $n 's'
$R wait $n 'mem · work · 0 starred'
$R type $n '1'
$R type $n 'p'
$R wait $n '❯ file expenses'
echo "mem: ok"
