#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. ./netpair.bash
pid=$($R broker $a)
$R stream $a s "$me:@b"
$R stream-wait $a s '"history":'
kill -9 "$pid"
$R say $b 'after the broker died' 'echo: after the broker died'
$R stream-wait $a s '"turn":"done"'
echo "netattach: ok"
