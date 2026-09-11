# Self-update

When the user gives standing feedback on orchestrator behavior ("from now
on…", "always…", "stop doing…"), change the skill itself, not just this
session:

1. Edit the matching file under `~/.claude/skills/orchestrate/` — the
   contract for role/boundary changes, the reference for procedure changes.
   Keep the contract thin; new procedures become reference sections, not
   contract text.
2. Append an `update` line to log.jsonl saying what changed and why.
3. Confirm in one sentence. The change takes effect next skill load; note
   that if it matters.

Never weaken the hard rules (workers don't orchestrate, dispatch instead of
implementing, TTS-sized replies) without the user saying so explicitly.
