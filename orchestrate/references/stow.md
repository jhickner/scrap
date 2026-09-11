# Stow

Persist what this session learned; run when the user says "stow", at the end
of a work stretch, or before a long away period.

1. Durable knowledge — decisions made, constraints discovered, preferences
   stated, gotchas hit — goes to `~/.config/orchestrator/notes/<project>.md`,
   dated bullets, one line each. Facts the repo or ledger already records do
   not belong there.
2. Archive: move `done`/`failed` records older than ~7 days from
   `projects/<name>.jsonl` to `projects/<name>.archive.jsonl` (newest record
   per id, whole history for that id). Keep the live file small.
3. Trim consumed result files for archived tasks into the archive dir.
4. Confirm in one sentence: what was noted, how many records archived.
