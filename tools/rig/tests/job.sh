#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake)
trap '$R stop $n 2>/dev/null || true' EXIT
$R wait $n '❯'
jobs="$($R dir $n)/state/config/jobs"
mkdir -p "$jobs"
printf 'schedule = 0/2 * * * * ?\n\nscheduled hello\n' > "$jobs/greet.job"
$R joblog $n greet 'echo: scheduled hello'
echo "job: ok"
