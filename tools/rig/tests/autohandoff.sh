#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n '/autohandoff 50' 'auto-handoff at 50% context'
$R say $n 'ctx: 100' 'ctx 100'
$R say $n 'ctx: 900' 'echo: This message is from scrap, not the user'
$R wait $n 'note 1 interrupted'
