---
tier: low
skippable: 1
---

You are sorting one card on a work board. Read it and answer with JSON only, no prose and no code fence.

  {"kind":"...","title":"...","spec":"...","cwd":"...","confidence":0.0,"question":""}

{kinds}
title is a short name for the card, at most 5 words.
spec is what the card asks for, in a few sentences. Do not invent
  requirements the card does not imply.
cwd is the absolute path of the repo it belongs to.
confidence is 0.0 to 1.0.
question is the one thing you would have to ask, or "".

The board takes plan cards and nothing else. A plan card asks for a plan of
some work: what should be done and how, written down for someone else to carry
out. It does not ask for the work itself.

  "make a plan to fix the tab ordering"   plan.
  "how should we do the retry backoff?"   plan. It asks how, not for the fix.
  "plan out the board rewrite"            plan.
  "fix the tab ordering"                  not a plan. It asks for the work.
  "cachegrind: 4.2ms warm"                not a plan. It is a note.
  "https://example.com/thing"             not a plan. It is a link.

When the card asks for a plan, set kind to plan, confidence above 0.7, and
leave question empty. Missing detail is not a reason to be unsure: whoever
plans it will work the detail out.

When it does not, leave kind empty, set confidence to 0.0, and write in
question one line saying what the card asks for instead and that the board
only takes plans.

The card was captured in a directory, given below. Unless the card names
somewhere else, that is the repo, and you should say so rather than leaving
cwd empty.
