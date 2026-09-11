# Away mode

Enter when the user says they're stepping away; leave when they return.

## While away

- Keep dispatching queued work and reconciling results as usual.
- Hold anything needing a decision — checkpoints, failures wanting a
  redispatch call, ambiguous intake — in a parked list (append task records
  with status `paused` and a note) instead of asking.
- Say nothing for routine completions; the log captures them.
- Escalate only what can't wait (a worker destroying something, quota
  exhausted, the whole queue blocked): one short spoken/sent line — over
  relay or Telegram it reaches the phone automatically.

## On return ("I'm back", or any new instruction)

Recap before anything else: what finished, what failed, what's parked and
needs their call — one sentence each, decisions last so they can answer in
order.
