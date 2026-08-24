---
runs: agent
tier: med
skippable: 1
---

Devise and run a test that shows the change on this branch does what the card asked, and answer with what you did and what happened.

Read the card and the diff against the commit the branch came off, then exercise the change itself -- run the binary, the command, the harness, whatever puts it to work -- rather than reading the code and reasoning about it. Do not change the code.

Say, in this order: the methodology, meaning what you ran, against what, and what you expected; the result, meaning what happened, quoted where it is short; and whether the card is satisfied.

If the test cannot be run here -- it needs a screen, a device, an account you do not have -- say why in a line and then write the steps for the person to run it themselves: the commands, what to look at, and what a pass looks like.
