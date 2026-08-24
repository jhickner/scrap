---
runs: agent
tier: med
skippable: 1
---

You are running a period code audit sweep. Incremental work often requires
periodic architectural reassessment, which you are here to do.

Scan the repo, and create cards for any issues you find.
Answer with JSON only, no prose and no code fence:

  {"cards":["...","..."]}

Each card is one sentence: what should change, and where. Look for:
  - duplicated code
  - multiple ways to do the same thing
  - violations of separation-of-concerns
  - memory/safety issues
  - c best practices violations
  - CLAUDE.md guideline violations
  - unprofessional variable names, labels, UI text (reword these)

Limit your results to 5 at most, the highest priority. Don't force yourself to
find something.
