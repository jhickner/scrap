#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
S=$PWD/../../scrap
H=$(mktemp -d); trap 'rm -rf "$H"' EXIT
fail() { echo "agentsync: $*" >&2; exit 1; }
js() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if eval(sys.argv[2]) else 1)' "$H/$1" "$2" || fail "$1: $2"; }
cd "$H"
mkdir -p .claude/skills/a .codex/skills/a .codex/skills/own .codex/skills/.system .pi/agent/skills .grok/skills .codex
echo canonical > .claude/skills/a/SKILL.md
echo stale > .codex/skills/a/SKILL.md
echo codex > .codex/skills/own/SKILL.md
ln -s "$H/.codex/skills/a" .pi/agent/skills/a
ln -s "$H/.claude/skills/a" .grok/skills/a
ln -s /nonexistent/elsewhere .grok/skills/foreign
cat > .claude/settings.json <<J
{"model":"opus","hooks":{"SessionStart":[{"hooks":[{"type":"command","command":"hook.py"}]}],"Notification":[{"matcher":"","hooks":[{"type":"command","command":"hook.py"}]}]}}
J
cat > .codex/hooks.json <<J
{"hooks":{"SessionStart":[{"hooks":[{"type":"command","command":"codex-hook.py"}]}],"PostToolUse":[{"hooks":[{"type":"command","command":"codex-hook.py"}]}]}}
J
sync() { HOME=$H SCRAP_CONFIG_DIR=$H/cfg "$S" sync "$@" >/dev/null || fail "sync $* failed"; }
sync
readlink .agents/skills | grep -q '/\.claude/skills' || fail "agents/skills target"
[ ! -e .pi/agent/skills/a ] && [ ! -L .pi/agent/skills/a ] || fail "pi link to codex copy kept"
[ ! -L .grok/skills/a ] || fail "grok link kept"
[ -L .grok/skills/foreign ] || fail "foreign link removed"
[ -e .codex/skills/a/SKILL.md ] || fail "codex a removed without prune"
[ -e .codex/skills/own/SKILL.md ] || fail "codex own removed"
[ -e .codex/skills/.system ] || fail ".system removed"
js .config/scrap/hooks.json "d['hooks']['SessionStart'][0]['hooks'][0]['codex']=='codex-hook.py' and 'Notification' in d['hooks']"
js .claude/settings.json "d['model']=='opus' and 'Notification' in d['hooks'] and 'codex-hook.py' not in json.dumps(d) and 'PostToolUse' not in d['hooks']"
js .codex/hooks.json "'codex-hook.py' in json.dumps(d) and 'Notification' not in d['hooks'] and '\"codex\"' not in json.dumps(d) and 'PostToolUse' in d['hooks']"
grep -q session_start .pi/agent/extensions/scrap-hooks.ts || fail "pi extension"
cat > .config/scrap/hooks.json <<J
{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"all.sh","claude":"claude.sh","pi":false}]}],"SessionEnd":[{"hooks":[{"type":"command","command":"end.sh","codex":false}]}]}}
J
sync --prune
js .claude/settings.json "'claude.sh' in json.dumps(d) and 'all.sh' not in json.dumps(d) and 'end.sh' in json.dumps(d) and '\"pi\"' not in json.dumps(d)"
js .codex/hooks.json "'all.sh' in json.dumps(d) and 'SessionEnd' not in d['hooks']"
[ ! -e .codex/skills/a ] || fail "prune kept stale codex a"
[ -e .codex/skills/own/SKILL.md ] || fail "prune removed codex own"
echo "agentsync: ok"
