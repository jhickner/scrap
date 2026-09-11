#!/usr/bin/env bash
# Read-only real quota snapshot for claude/codex/grok. Prints one JSON object:
#   {backend: {remaining_pct, resets_at, read_at, source}}
# remaining_pct/resets_at are null when no reading is available.
#
# Sources:
#   claude/codex/grok: mux's agenttabs state dir, written by claude.h/codex.h/grok.h
#                 rate_limit hooks whenever mux runs those backends. Falls back to
#                 scanning codex CLI session jsonl (~/.codex/sessions) for codex only.
set -euo pipefail

now=$(date +%s)

state_dir() {
    if [ -n "${AGENT_TABS_STATE_DIR:-}" ]; then
        echo "$AGENT_TABS_STATE_DIR"
        return
    fi
    local base="$HOME/.config/tmux"
    local legacy="$HOME/.tmux"
    if [ ! -d "$base" ] && [ -d "$legacy" ]; then
        base="$legacy"
    fi
    echo "$base/tmux-agent-tabs"
}

agents_dir="$(state_dir)/agents"

# Latest usage reading for one backend name from mux's agent-tab records.
# Emits {remaining_pct, resets_at, read_at, source} or "null".
latest_from_agenttabs() {
    local backend="$1"
    [ -d "$agents_dir" ] || { echo "null"; return; }
    jq -s --arg backend "$backend" --arg src "mux agenttabs: $agents_dir" '
        map(select(.agent == $backend and (.usage_percent != null)))
        | sort_by(.usage_ts) | last
        | if . == null then null else {
            remaining_pct: (100 - .usage_percent),
            resets_at: .usage_resets_at,
            read_at: .usage_ts,
            source: $src
          } end
    ' "$agents_dir"/*.json 2>/dev/null || echo "null"
}

# Fallback for codex: scan recent session jsonl files for the latest
# token_count event's rate_limits.primary block.
latest_from_codex_sessions() {
    local sessions_dir="$HOME/.codex/sessions"
    [ -d "$sessions_dir" ] || { echo "null"; return; }
    local f
    f=$(find "$sessions_dir" -iname '*.jsonl' -newermt '-7 days' -print0 2>/dev/null \
        | xargs -0 ls -t 2>/dev/null | head -1)
    [ -n "$f" ] || { echo "null"; return; }
    /usr/bin/grep '"rate_limits"' "$f" 2>/dev/null | tail -1 | jq --arg src "codex session jsonl: $f" '
        .payload.rate_limits.primary as $p
        | if $p == null then null else {
            remaining_pct: (100 - $p.used_percent),
            resets_at: $p.resets_at,
            read_at: (.timestamp | fromdateiso8601),
            source: $src
          } end
    ' 2>/dev/null || echo "null"
}

claude=$(latest_from_agenttabs claude)
codex=$(latest_from_agenttabs codex)
if [ "$codex" = "null" ]; then
    codex=$(latest_from_codex_sessions)
fi
grok=$(latest_from_agenttabs grok)

jq -n --argjson claude "$claude" --argjson codex "$codex" --argjson grok "$grok" \
    '{claude: $claude, codex: $codex, grok: $grok}'
