#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
state=$(mktemp -d /tmp/scraprig-bridges.XXXXXX)
mkdir -p "$state/config"
port=$((20000 + RANDOM % 20000))
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' "$port" > "$state/config/relay"
a=$($R start --fake -- --state "$state")
b=$($R start --fake -- --state "$state")
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$state"' EXIT
$R wait $a '❯'
$R wait $b '❯'
$R say $a '/relay on' 'relay on in this window'
$R say $b '/relay on' 'relay on in another window'
$R stop $a
$R wait $b "relay on ws://127.0.0.1:$port" 10
$R say $b '/relay' 'relay off'
grep -q '^relay=0$' "$state/config/settings"
echo "bridges: ok"
