#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
notes() { ls /tmp/scraprig/$n/state/config/handoffs 2>/dev/null | wc -l | tr -d ' '; }
expect_notes() {
    for _ in $(seq 50); do [ "$(notes)" = "$1" ] && return; sleep 0.2; done
    $R snap $n; echo "expected $1 archived notes, found $(notes)" >&2; exit 1
}
$R wait $n '❯'
$R say $n '/autohandoff 50' 'auto-handoff at 50% context'
$R say $n 'ctx: 100' 'ctx 100'
$R type $n 'ctx: 900 chain'
$R send $n Enter
expect_notes 2
$R idle $n
expect_notes 2
$R type $n 'ctx: 900'
$R send $n Enter
expect_notes 3
