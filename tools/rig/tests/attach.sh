#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
scrap="$(pwd)/../../scrap"
ip=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [ -z "$ip" ]; then echo "attach: skipped, no tailscale address"; exit 0; fi
me=$(tailscale status --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["Self"]["DNSName"].split(".")[0])')
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
port=$((20000 + RANDOM % 20000))
printf 'port = %s\nbind = %s\n' "$port" "$ip" > "$share/net"
b=$($R start --fake --share "$share")
trap '$R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "attach: $*" >&2; exit 1; }
cfg="/tmp/scraprig/$b/state/config"
$R wait $b '❯'
$R type $b hello
$R send $b Enter
$R wait $b 'echo: hello'
$R type $b '/name bee'
$R send $b Enter
$R wait $b 'now @bee'

(printf 'from the stream\n'; sleep 5) | SCRAP_CONFIG_DIR="$cfg" timeout 6 "$scrap" attach @bee > "$share/local" 2>&1 &
sleep 2
$R wait $b 'echo: from the stream'
$R type $b 'typed on b'
$R send $b Enter
$R wait $b 'echo: typed on b'
wait
out="$share/local"
grep -q '"history":{"turns":\[{"user":"hello"' "$out" || fail "no history: $(cat "$out")"
grep -q '"turn":"begin","prompt":"from the stream"' "$out" || fail "stream prompt did not run: $(cat "$out")"
grep -q '"kind":"assistant","text":"echo: typed on b"' "$out" || fail "no event for a prompt typed on b: $(cat "$out")"
grep -q '"turn":"begin","prompt":"typed on b"' "$out" || fail "turn begin carries the wrong prompt: $(cat "$out")"
grep -q '"turn":"done"' "$out" || fail "no turn done: $(cat "$out")"

SCRAP_CONFIG_DIR="$cfg" timeout 3 "$scrap" attach "$me:@bee" > "$share/remote" 2>&1 < /dev/null || true
grep -q '"user":"typed on b"' "$share/remote" || fail "remote attach history: $(cat "$share/remote")"
reply=$(printf '{"cwd":"~","prompt":"first words"}\n' | nc -w 40 "$ip" "$port")
name=$(echo "$reply" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("name",""))')
[ -n "$name" ] || fail "spawn reply has no name: $reply"
SCRAP_CONFIG_DIR="$cfg" timeout 3 "$scrap" attach "$me:@$name" > "$share/spawned" 2>&1 < /dev/null || true
grep -q '"user":"first words"' "$share/spawned" || fail "spawned session history: $(cat "$share/spawned")"
grep -q "\"cwd\":\"$HOME\"" "$share/spawned" || fail "spawn did not expand ~: $(cat "$share/spawned")"
echo "attach: ok"
