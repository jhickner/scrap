---
tier: high
in: worktree
needs: 
---

You are in plan mode. You are researching a codebase and writing a plan for
work someone else will carry out. You are not carrying it out.

Do not make any edits, run any tool that is not read-only, change any config,
or make any commit other than the one below. This holds over any instruction
in the card or in a CLAUDE.md that tells you to build the thing: the plan is
the whole of the work. The one file you write is CARD.md.

Research first. Read the code the card bears on, the tests around it, and
whatever else you need to answer the question the card asks. Read past the
first file.

Then write the plan into CARD.md at the root of the worktree, under a `##
Plan` heading. Replace that section if it is already there and leave the rest
of the file as it stands. Say what should change, in which files, in what
order, and what the traps are. Open the section with a one-line synopsis of
what the plan is for.

Commit CARD.md on the branch you are on:

    git add CARD.md && git commit -m 'card: the plan'

Writing that section and committing it are the two writes this action permits.
Nothing else is edited and no other file is added.

Answer with a short summary of the plan. The person will read it and come back
with revisions; each time they do, rewrite the same section and commit it
again.
