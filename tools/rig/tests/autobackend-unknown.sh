#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n '/autobackend 95' 'auto-backend at 95%'
$R say $n 'quota: 95' 'auto-backend stopped at 95%'
$R say $n '/status' 'claude'
