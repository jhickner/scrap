#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake --openai -s 120x40 -- -b core -m fake/echo); trap '$R stop $n' EXIT
$R wait $n '❯'
$R say $n 'run: sleep 4; echo done' '\[bash\] sleep 4'
$R say $n 'go left' 'steer · next tool step · go left'
$R wait $n 'steered: go left' 15
$R idle $n
file=$(ls -t "$($R dir $n)"/state/config/agent/sessions/*/*.jsonl | head -1)
order=$(python3 -c '
import json, sys
for line in open(sys.argv[1]):
    m = json.loads(line).get("message") or {}
    c = m.get("content")
    if isinstance(c, list):
        c = " ".join(b.get("text", "") for b in c if isinstance(b, dict))
    print(m.get("role", "-"), (c or "")[:40].replace("\n", " "))
' "$file")
grep -A1 '^tool ' <<<"$order" | grep -q '^user go left' ||
    { echo "steerresume: want the steer right after the tool result" >&2; echo "$order" >&2; exit 1; }
grep -A1 '^user go left' <<<"$order" | grep -q '^assistant steered: go left' ||
    { echo "steerresume: want the reply after the steer" >&2; echo "$order" >&2; exit 1; }
echo "steerresume: ok"
