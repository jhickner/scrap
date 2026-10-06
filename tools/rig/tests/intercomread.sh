#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scrap="$(pwd)/../../scrap"
tmp=$(mktemp -d /tmp/scraprig-read.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
tmp=$(cd "$tmp" && pwd -P)
work="$tmp/work"
proj="$tmp/home/.claude/projects/$(printf '%s' "$work" | tr -c 'a-zA-Z0-9-' '-')"
mkdir -p "$work/sub" "$proj" "$tmp/config"
cp fixtures/past.jsonl "$proj/aaaa1111-named.jsonl"
cp fixtures/past.jsonl "$proj/bbbb2222-old.jsonl"
printf 'aaaa1111-named\tamber-fox\tclaude\t%s\n' "$work" > "$tmp/config/names"
run() { (cd "$work/sub" && HOME="$tmp/home" SCRAP_CONFIG_DIR="$tmp/config" "$scrap" "$@"); }
fail() { echo "intercomread: $*" >&2; exit 1; }

out=$(run ls --exited --cwd "$work")
echo "$out" | grep -Eq '^@amber-fox +past ' || fail "ls lacks @amber-fox: $out"
echo "$out" | grep -Eq '^bbbb2222 +past .*where is the retry backoff' || fail "ls lacks the unnamed session: $out"
[ -z "$(run ls)" ] || fail "ls lists past sessions"
run ls --exited zebracorn | grep -q amber-fox || fail "ls QUERY does not search transcripts"
[ -z "$(run ls --exited --cwd "$work/sub")" ] || fail "ls --cwd matches outside DIR"

out=$(run read @amber-fox -n 1)
echo "$out" | grep -q 'last 1 of 2 turns' || fail "read header: $out"
echo "$out" | grep -q 'zebracorn' || fail "read lacks the last turn: $out"
if echo "$out" | grep -q 'RETRY_BASE_MS\.'; then fail "read -n 1 printed an older turn: $out"; fi
run read bbbb | grep -q 'raise it to 500' || fail "read by id prefix"
run read 'retry backoff' | grep -q 'raise it to 500' || fail "read by title"
out=$(run send @amber-fox hi 2>&1 || true)
echo "$out" | grep -q 'scrap open @amber-fox' || fail "send to a past session: $out"
if run read @nobody 2>/dev/null; then fail "read of an unknown target succeeds"; fi
echo "intercomread: ok"
