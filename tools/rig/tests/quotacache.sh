#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake); trap '$R stop $n' EXIT
f="$($R dir $n)/state/config/quota/claude.json"
$R wait $n '❯'
$R say $n 'quota: 25' 'quota 25'
for _ in $(seq 25); do grep -q '"used_percent":25' "$f" 2>/dev/null && break; sleep 0.2; done
grep -q '"used_percent":25' "$f" || { cat "$f" 2>/dev/null; echo "claude quota not cached" >&2; exit 1; }
$R say $n 'quota: 60' 'quota 60'
for _ in $(seq 25); do grep -q '"used_percent":60' "$f" 2>/dev/null && break; sleep 0.2; done
grep -q '"used_percent":60' "$f" || { cat "$f"; echo "changed reading not cached" >&2; exit 1; }
