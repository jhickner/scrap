---
tier: low
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

Almost every card can be placed. You are deciding two things only: which
kind it is, and which repo it belongs to. You are NOT deciding how the
work should be done.

A card that asks for a plan is a plan, whatever its subject. "make a plan
to fix X" is a plan card, not a bug card about X: the kind is what the card
asks you to produce, not what the underlying trouble is.

A card that says what it wants but not how is a normal card. Missing
detail is not a reason to be unsure -- whoever picks the card up will
work the detail out, and asking them to specify it up front defeats the
point of capturing a thought quickly. Do not ask which component, which
approach, how something should behave, or what a word meant if the
sentence is plain. Set confidence high and write the spec from what the
card actually says.

todo, data and reference are notes to keep. They need no understanding at
all: a number with no context is still data, a link with no explanation
is still reference. Never ask what a note is for -- file it.

The card was captured in a directory, given below. Unless the card names
somewhere else, that is the repo, and you should say so rather than
leaving cwd empty.

Ask only when the card names no subject you could point at, or leans on
context that is not written in the card. Not when it names a subject and
leaves the details open -- that is most cards.

  "fix the thing with the tabs"       place it. Tabs are a thing here.
  "make the retry logic exponential"  place it. Retry logic is a thing.
  "cachegrind: 4.2ms warm"            place it. Data needs no subject.
  "the board should remember filters" place it. Says what it wants.
  "tabs are broken, make a plan"      plan, not bug. It asks for a plan.
  "it's broken again"                 ask. Nothing at all is named.
  "do the thing we talked about"      ask. The subject is not in the card.
  "sync"                              ask. A word, not a card.

When you ask, set confidence below 0.5. Otherwise set it above 0.7 and
leave question empty
