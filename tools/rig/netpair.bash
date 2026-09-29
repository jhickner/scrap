test_name=$(basename "$0" .sh)
R=./scraprig
tailscale ip -4 >/dev/null 2>&1 || { echo "$test_name: skipped, no tailscale address"; exit 0; }
fail() { echo "$test_name: $*" >&2; exit 1; }
until_ok() { for _ in $(seq 60); do "$@" >/dev/null 2>&1 && return 0; sleep 0.25; done; "$@"; }
me=$($R machine)
share=$(mktemp -d /tmp/scraprig-share.XXXXXX)
a=$($R start --fake --share "$share" --net)
b=$($R start --fake --share "$share" --net)
extra=""
trap '$R stop $a 2>/dev/null || true; $R stop $b 2>/dev/null || true; for n in $extra; do $R stop $n 2>/dev/null || true; done; rm -rf "$share"' EXIT
for n in $a $b; do
    $R wait $n '❯'
    $R say $n hello 'echo: hello'
done
$R say $a '/name a' 'this session is now @a'
$R say $b '/name b' 'this session is now @b'
until_ok $R cli $a send "$me:@b" warmup
$R wait $b 'warmup'
