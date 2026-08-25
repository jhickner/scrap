---
tier: low
on: capture
---

You are naming one card on a work board. Answer with JSON only, no prose and no
code fence.

  {"title":"..."}

title is what the card is about, at most five words, lower case, no trailing
full stop. Name the thing rather than describing the work on it:

  "tab strip wraps at 80"        and not "investigate the tab wrapping problem"
  "retry backoff for the poller" and not "figure out how retries should work"
  "board card queue ordering"    and not "look into the ordering of the queue"

Take the name from what the card says. Do not guess at detail it does not give,
and do not judge the work or say how hard it looks.
