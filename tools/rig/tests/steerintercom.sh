#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share")
b=$($R start --fake --openai --share "$share" -s 120x40 -- -b core -m fake/echo)
trap '$R stop $a; $R stop $b; rm -rf "$share"' EXIT
fail() { echo "steerintercom: $*" >&2; $R snap $b >&2; exit 1; }
$R wait $a '❯'
$R wait $b '❯'
$R say $a hello 'echo: hello'
$R say $a '/name a' 'this session is now @a'
$R say $b '/name b' 'this session is now @b'

out=$($R cli $a send --steer --interrupt @b hi 2>&1 || true)
grep -q 'usage: scrap send \[--interrupt | --steer\]' <<<"$out" || fail "steer with interrupt: $out"

$R say $b 'run: sleep 4; echo done' '\[bash\] sleep 4'
out=$($R cli $a send --from a --steer @b go left 2>&1)
[ "$out" = "sent to @b (steered)" ] || fail "busy: $out"
$R wait $b 'steered: \[from @a\] go left' 15
$R idle $b
screen=$($R snap $b)
grep -q '^▌ from @a: go left$' <<<"$screen" || fail "no user block for the steer"
[ "$(grep -cE '^[0-9]+s( · |$)' <<<"$screen")" = 1 ] || fail "want one turn"

out=$($R cli $a send --from a --steer @b now idle 2>&1)
[ "$out" = "sent to @b (delivered: idle, started a turn)" ] || fail "idle: $out"
$R wait $b 'echo: \[from @a\] now idle' 15
$R idle $b

$R say $b 'run: sleep 6; echo done' '\[bash\] sleep 6'
out=$($R cli $a send --from a --steer @b two 2>&1)
[ "$out" = "sent to @b (steered)" ] || fail "second steer: $out"
out=$($R cli $a send --from a --steer @b three 2>&1)
[ "$out" = "sent to @b (queued: too many steers to this session in the last minute)" ] || fail "cap: $out"
$R wait $b 'steered: \[from @a\] two' 20
$R wait $b '^▌ from @a( \(answered\))?: three$' 20
$R idle $b
echo "steerintercom: ok"
