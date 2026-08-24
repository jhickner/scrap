---
runs: agent
tier: med
skippable: 1
fail marker: BROKEN
fail step: worktree
---

Devise and run a test that shows the change on this branch does what the card asked, and answer with what you did and what happened.

Read the card and the diff against the commit the branch came off, then exercise the change itself -- run the binary, the command, the harness, whatever puts it to work -- rather than reading the code and reasoning about it. Do not change the code.

Say, in this order: the methodology, meaning what you ran, against what, and what you expected; and the result, meaning what happened, quoted where it is short.

End on a line carrying one of these words alone:

  WORKS      a test was run and the card is satisfied
  BROKEN     a test was run and the change does not do what the card asked
  UNTESTED   the test needs a screen, a device, or an account you do not have

BROKEN sends the card back to the worker that built it, so it is for a test that ran and came out wrong. A test you could not run here is UNTESTED, which is not a failure: say in one line what stopped you, and write the steps for the person to run it themselves -- the commands, what to look at, and what a pass looks like. A card no test can settle is theirs to settle, not yours to reject.

Do not write BROKEN anywhere but that last line. Where output you are quoting carries the word, describe the output instead.
