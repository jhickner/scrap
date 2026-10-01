#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'hello' 'echo: hello'
$R say $n 'run: t=${TMPDIR:-/tmp}; f=${t%/}/scrap-handoff-fake-$PPID.md; printf "next step: ship it" > $f; echo saved; echo "@handoff $f"' 'echo: next step: ship it'
$R wait $n 'handoff ready'
$R wait $n '› handoff|❯ handoff|handoff$'
