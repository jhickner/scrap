#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "attach: skipped, no tailscale address"; exit 0; }
me=$($R machine)
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
b=$($R start --fake --share "$share" --net)
trap '$R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "attach: $*" >&2; exit 1; }
$R wait $b '❯'
$R say $b hello 'echo: hello'
$R say $b '/name bee' 'now @bee'

(printf 'from the stream\n'; sleep 5) | timeout 6 $R cli $b attach @bee > "$share/local" 2>&1 &
sleep 2
$R wait $b 'echo: from the stream'
$R say $b 'typed on b' 'echo: typed on b'
wait
out="$share/local"
grep -q '"history":{"turns":\[{"user":"hello"' "$out" || fail "no history: $(cat "$out")"
grep -q '"turn":"begin","prompt":"from the stream"' "$out" || fail "stream prompt did not run: $(cat "$out")"
grep -q '"kind":"assistant","text":"echo: typed on b"' "$out" || fail "no event for a prompt typed on b: $(cat "$out")"
grep -q '"turn":"begin","prompt":"typed on b"' "$out" || fail "turn begin carries the wrong prompt: $(cat "$out")"
grep -q '"turn":"done"' "$out" || fail "no turn done: $(cat "$out")"

timeout 3 $R cli $b attach "$me:@bee" > "$share/remote" 2>&1 < /dev/null || true
grep -q '"user":"typed on b"' "$share/remote" || fail "remote attach history: $(cat "$share/remote")"
read -r ip port < <($R net $b)
reply=$(printf '{"cwd":"~","prompt":"first words"}\n' | nc -w 40 "$ip" "$port")
name=$(echo "$reply" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("name",""))')
[ -n "$name" ] || fail "spawn reply has no name: $reply"
timeout 3 $R cli $b attach "$me:@$name" > "$share/spawned" 2>&1 < /dev/null || true
grep -q '"user":"first words"' "$share/spawned" || fail "spawned session history: $(cat "$share/spawned")"
grep -q "\"cwd\":\"$HOME\"" "$share/spawned" || fail "spawn did not expand ~: $(cat "$share/spawned")"
echo "attach: ok"
