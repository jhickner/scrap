#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./muxrig
n=$($R start --fixture fixtures/toolcall.jsonl); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n 'list files'
$R send $n Enter
$R wait $n 'Two files: alpha.txt and beta.txt'
$R wait $n '\[bash\] ls'
