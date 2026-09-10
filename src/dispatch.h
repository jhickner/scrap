#ifndef DISPATCH_H
#define DISPATCH_H

/* agent-facing session control: a JSON request dropped at
   ~/.config/mux/dispatch/<pid>-<id>.req asks this instance to open a new
   workspace session ({"backend","model","effort","cwd","prompt"}, all
   optional), or send a line to one ({"slot":N,"send":"line"}); the reply
   lands beside it as <pid>-<id>.res. children see this instance's pid as
   $MUX_PID. */
void dispatch_poll(void);

#endif
