#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tailscale ip -4 >/dev/null 2>&1 || { echo "hubquiet: skipped, no tailscale address"; exit 0; }
tmp=$(mktemp -d /tmp/scraprig-hub.XXXXXX)
mkdir -p "$tmp/config" "$tmp/tmp"
SCRAP_CONFIG_DIR="$tmp/config" TMPDIR="$tmp/tmp" ../../scrap hub > "$tmp/out" &
pid=$!
trap 'kill $pid 2>/dev/null || true; rm -rf "$tmp"' EXIT
sleep 3
kill -0 $pid || { echo "hubquiet: hub exited" >&2; exit 1; }
if grep -q 'scrap hub: .*[0-9]:[0-9]' "$tmp/out"; then
    echo "hubquiet: a hub with its own config dir and no port uses the tailnet: $(cat "$tmp/out")" >&2
    exit 1
fi
echo "hubquiet: ok"
