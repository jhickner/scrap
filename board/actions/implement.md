---
tier: med
in: worktree
commits: yes
needs: 
---

Read CARD.md at the root of the worktree first. Its `## Plan` section was
approved and is the spec; it supersedes the card body inlined below, which is
only what the card was opened with. If there is no such section, the card body
is all there is.

Start coding. Start with your todo list.

Work to the plan rather than to your own reading of the card: if a step of it
looks wrong, say so and carry on with the rest, and do not redesign it midway.
What the plan does not settle is yours to settle, in the style of the code
around it.

You are in a worktree of your own, on a branch of its own. Do not merge, do not
switch branches, do not touch the main branch, and do not start work the plan
does not ask for.

Reach a state someone else can test, commit it to the branch you are already
on, and then stop and say how to test it. Commit even when the work is
unfinished: uncommitted work does not exist to anything downstream. CARD.md is
tracked on the branch too — commit it with the rest if you wrote to it.

The CLAUDE.md files in scope are binding, not advisory. Two rules they state
are broken most often, so they are repeated here as tests to apply to your own
diff before you commit:

Comments. Default to none. A comment may record why something is as it is -- a
constraint, a trap, a decision that looks wrong and is not. It may not say what
the code does; the code says that. Before keeping one, delete it and ask what a
reader lost: if the answer is nothing, leave it deleted.

Commits. Read the last twenty messages in the log and write like them. The
subject is `area: what changed`, lower case, no trailing full stop, naming the
change rather than passing judgement on it. Add a body only where the subject
cannot carry it, and then it is more of what changed -- the functions, files and
behaviour added, removed or replaced -- not an argument for the change. No
co-author trailers and no attribution to a tool.

Answer with what you built, which files it touched, and what to run to see it
work.
