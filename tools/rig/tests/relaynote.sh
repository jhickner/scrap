#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
R=./scraprig
a=$($R start --fake)
trap '$R stop $a' EXIT
bin="$($R dir $a)/bin"
rm "$bin/mem"
cat > "$bin/mem" <<MEM
#!/bin/sh
case " \$* " in
  *" stores "*) echo '{"stores":["personal"]}' ;;
  *" tags "*) echo '{"tags":[]}' ;;
  *" create "*) echo create >> "$bin/log"; sleep 1; echo '{"id":"x1","title":null,"content":"hi","tags":[]}' ;;
esac
MEM
chmod +x "$bin/mem"
printf 'token = rigtesttoken0123456789\nbind = 127.0.0.1\nport = %d\n' \
    $((20000 + RANDOM % 20000)) > "$($R dir $a)/state/config/relay"
$R wait $a '❯'
$R say $a '/relay on' 'relay on ws'

# a note resent while running, and again after, is saved once; mem requests still run
$R relay $a '[
  {"wait": "\"t\":\"view\""},
  {"t": "req", "op": "note", "note": "n1", "text": "buy feed", "id": "a1"},
  {"t": "req", "op": "note", "note": "n1", "text": "buy feed", "id": "a2"},
  {"wait": "\"id\":\"a2\",\"ok\":true,\"mem\":\\{\"id\":\"x1\".*\"store\":\"personal\"", "secs": 30},
  {"t": "req", "op": "note", "note": "n1", "text": "buy feed", "id": "a3"},
  {"wait": "\"id\":\"a3\",\"ok\":true,\"mem\":\\{\"id\":\"x1\""},
  {"t": "req", "op": "note", "text": "no id", "id": "a4"},
  {"wait": "\"id\":\"a4\",\"ok\":false,\"code\":\"invalid\""},
  {"t": "req", "op": "mem", "store": "", "args": ["stores"], "id": "m1"},
  {"wait": "\"id\":\"m1\",\"ok\":true,\"mem\":\\{\"stores\":\\[\"personal\"\\]"}
]' >/dev/null
[ "$(wc -l < "$bin/log")" -eq 1 ] || { echo "relaynote: saved $(wc -l < "$bin/log") times"; exit 1; }
echo "relaynote: ok"
