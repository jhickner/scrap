#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
state=$(mktemp -d /tmp/scraprig-bridges.XXXXXX)
mkdir -p "$state/config"
port=$((20000 + RANDOM % 20000))
printf 'token = rigtesttoken0123456789abcdefghijklmn\nbind = 127.0.0.1\nport = %d\n' "$port" > "$state/config/api"
a=$($R start --fake -- --state "$state")
b=$($R start --fake -- --state "$state")
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$state"' EXIT
$R wait $a '❯'
$R wait $b '❯'
$R say $a '/api on' 'api on in this window'
$R say $b '/api on' 'api on in another window'
$R stop $a
for _ in $(seq 20); do nc -z 127.0.0.1 "$port" 2>/dev/null && break; sleep 0.5; done
nc -z 127.0.0.1 "$port"
$R say $b '/api' 'api off'
grep -q '^api=0$' "$state/config/settings"
echo "bridges: ok"
