#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4' '\[bash\] sleep 4'
$R say $n 'queued one' '▌ queued one'
$R send $n Tab
$R type $n x
sleep 0.5
if $R snap $n | grep -q 'queued one'; then
    $R snap $n >&2; echo "queuefocus: x did not drop the focused line" >&2; exit 1
fi
$R wait $n 'ran:' 10
sleep 2
if $R snap $n | grep -q 'echo: queued one'; then
    $R snap $n >&2; echo "queuefocus: the dropped line was sent" >&2; exit 1
fi
echo "queuefocus: ok"
