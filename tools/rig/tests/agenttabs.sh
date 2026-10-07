#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
n=$($R start --fake -- --name one); trap '$R stop $n' EXIT
d=$($R dir $n)/state/tabs/agents
fail() { echo "agenttabs: $*" >&2; exit 1; }
rec() {
    local f=$(ls "$d"/*-$1.json 2>/dev/null) || return 1
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if eval(sys.argv[2]) else 1)' "$f" "$2"
}
until_rec() {
    for _ in $(seq 50); do rec "$1" "$2" 2>/dev/null && return 0; sleep 0.2; done
    cat "$d"/*-$1.json 2>/dev/null || true
    fail "record $1: $2"
}
$R wait $n '❯'
until_rec 0 "d['agent']=='claude' and d['status']=='finished' and d['ts']>0 and 'usage_percent' not in d and 'provider' not in d"
$R type $n 'run: sleep 2'; $R send $n Enter
until_rec 0 "d['status']=='working'"
until_rec 0 "d['status']=='finished'"
$R say $n '/new two' 'echo: two'
$R say $n '/name two' 'now @two'
until_rec 1 "d['agent']=='claude' and d['status']=='finished'"
rec 0 "d['agent']=='claude' and d['status']=='finished'" || fail "second session overwrote the first"
$R say $n 'quota: 25' 'quota 25'
until_rec 1 "d['usage_percent']==25 and d['usage_resets_at']==2000000000 and d['usage_window_minutes']==300 and d['usage_ts']>0"
rec 0 "'usage_percent' not in d" || fail "quota leaked to the first record"
$R type $n 'run: sleep 2'; $R send $n Enter
until_rec 1 "d['status']=='working' and d['usage_percent']==25"
until_rec 1 "d['status']=='finished' and d['usage_percent']==25"
$R say $n '/backend codex' 'codex'
until_rec 1 "d['agent']=='codex' and 'usage_percent' not in d and 'usage_resets_at' not in d and 'usage_ts' not in d"
env -u SCRAP_PID $R cli $n close @one >/dev/null
for _ in $(seq 50); do ls "$d"/*-0.json >/dev/null 2>&1 || break; sleep 0.2; done
! ls "$d"/*-0.json >/dev/null 2>&1 || fail "closed session left its record"
rec 1 "d['agent']=='codex'" || fail "closing one session dropped the other's record"
echo "agenttabs: ok"
