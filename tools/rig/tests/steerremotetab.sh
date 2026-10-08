#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "steerremotetab: skipped, no tailscale address"; exit 0; }
me=mirror
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
printf 'port = %s\nbind = %s\ntailscale = %s\n' $((20000 + RANDOM % 20000)) "$(tailscale ip -4 | head -1)" "$(pwd)/fake-tailscale" > "$share/net"
a=$($R start --fake --share "$share" --net -s 120x40)
b=$($R start --fake --openai --share "$share" --net -s 120x40 -- -b core -m fake/echo)
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; rm -rf "$share"' EXIT
fail() { echo "steerremotetab: $*" >&2; $R snap $a >&2; exit 1; }
$R wait $a '❯'
$R wait $b '❯'
$R say $a hello 'echo: hello'
$R say $b hello 'echo: hello'
$R say $b '/name bee' 'now @bee'
$R say $a "/attach $me:@bee" "│ [^@]*$me:@bee *\$"
$R wait $a '▌ hello'

$R say $a 'run: sleep 5; echo done' '\[bash\] sleep 5'
$R say $a 'go right' 'steer · .* · go right'
$R wait $b 'steered: go right' 15
$R wait $a 'steered: go right' 15
$R idle $a
screen=$($R snap $a)
grep -q '^▌ go right$' <<<"$screen" || fail "no user block for the steer"
grep -q 'steer ·' <<<"$screen" && fail "steer row left behind"
[ "$(grep -c 'go right' <<<"$screen")" = 2 ] || fail "want the steer shown once and answered once"

$R say $a 'run: sleep 20; echo late' '\[bash\] sleep 20'
$R say $a 'after esc' 'steer · .* · after esc'
$R send $a Escape
$R wait $b '(echo|steered): after esc' 20
$R wait $a '(echo|steered): after esc' 20
$R idle $a
$R idle $b
[ "$($R snap $b | grep -cE '(echo|steered): after esc')" = 1 ] || { $R snap $b >&2; fail "the steer ran twice on the host"; }
echo "steerremotetab: ok"
