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

$R stream $b local @bee
$R stream-wait $b local '"history":\{"turns":\[\{"user":"hello"'
$R stream-put $b local 'from the stream'
$R wait $b 'echo: from the stream'
$R stream-wait $b local '"turn":"begin","prompt":"from the stream"'
$R say $b 'typed on b' 'echo: typed on b'
$R stream-wait $b local '"kind":"assistant","text":"echo: typed on b"'
$R stream-wait $b local '"turn":"begin","prompt":"typed on b"'
$R stream-wait $b local '"turn":"done"'

$R stream $b remote "$me:@bee"
$R stream-wait $b remote '"user":"typed on b"'
read -r ip port < <($R net $b)
reply=$(printf '{"cwd":"~","prompt":"first words"}\n' | nc -w 40 "$ip" "$port")
name=$(echo "$reply" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("name",""))')
[ -n "$name" ] || fail "spawn reply has no name: $reply"
$R stream $b spawned "$me:@$name"
$R stream-wait $b spawned '"user":"first words"'
$R stream-wait $b spawned "\"cwd\":\"$HOME\""
echo "attach: ok"
