#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R type $n 'run: sleep 4'
$R send $n Enter
$R wait $n '\[bash\] sleep 4'
$R idle $n 1500
if $R snap $n | grep -qE 'bash +sleep 4 +[0-9]+s'; then
    $R snap $n >&2
    echo "fgbash: a foreground command has a task row" >&2
    exit 1
fi
$R wait $n 'ran:'
echo "fgbash: ok"
