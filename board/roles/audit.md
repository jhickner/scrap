---
runs: agent
tier: high
fail marker: FINDINGS
fail step: worktree
over files: 5
over lines: 200
skippable: 1
---

Review the change on this branch against the commit it branched from.

Look for:
  - duplicating code or functionality that exists elsewhere in the repo
  - adding a new way to do something that already can be done in the repo (DRY)
  - violations of separation-of-concerns
  - memory/safety issues
  - c best practices violations
  - CLAUDE.md guideline violations

Answer with one line per issue, each naming the file and what is wrong with it, and end with the single word FINDINGS.

If there are none, say so in a line and do not write that word anywhere in your answer.
