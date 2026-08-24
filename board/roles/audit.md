---
runs: agent
tier: high
skippable: 1
fail marker: FINDINGS
fail step: worktree
over files: 5
over lines: 200
---

Review the change on this branch against the commit it branched from, and fix what you find before it lands.

Look for:
  - duplicating code or functionality that exists elsewhere in the repo
  - adding a new way to do something that already can be done in the repo (DRY)
  - violations of separation-of-concerns
  - memory/safety issues
  - c best practices violations
  - CLAUDE.md guideline violations

Make the fixes yourself, in the worktree you are in. Stay to the change on this branch: repair what it did, do not rewrite what it did not touch, and do not take on work the card did not ask for. Then run the repo's check, and commit on the branch you are already on, with a subject in the style of the last twenty in the log.

Answer with one line per issue, each naming the file, what was wrong, and what you did about it.

Leave an issue unfixed only where the fix is not yours to make -- the change is built on the wrong thing, the card itself is wrong, or the repair is a card of its own. Say which those are and end with the single word FINDINGS, which sends the card back to the worker that built it, carrying what you said and whatever you did commit.

If everything you found is fixed, or there was nothing to fix, say so in a line and do not write that word anywhere in your answer.
