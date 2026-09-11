#ifndef DISPATCH_H
#define DISPATCH_H

/* agent-facing session control: a JSON request dropped at
   ~/.config/mux/dispatch/<pid>-<id>.req asks this instance to open a new
   workspace session ({"backend","model","effort","cwd","prompt","title"},
   all optional; title is the session/tab name and is not replaced by
   auto-titling), send a line to one ({"slot":N,"send":"line"}), or close
   one by slot or session id ({"close":N} or {"close":"<id>"}; refused for
   the slot in view or one mid-turn, and later slots shift down); the reply
   lands beside it as <pid>-<id>.res. children see this instance's pid as
   $MUX_PID. */
void dispatch_poll(void);

#endif
