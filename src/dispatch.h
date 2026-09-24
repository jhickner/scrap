#ifndef DISPATCH_H
#define DISPATCH_H

/* agent-facing session control: a JSON request dropped at
   ~/.config/mux/dispatch/<pid>-<id>.req asks this instance to open a new
   workspace session ({"backend","model","effort","cwd","prompt","title"},
   all optional; title is the session/tab name and is not replaced by
   auto-titling), send a line to one ({"session":"<id>","send":"line"}), or
   close one ({"close":"<id>"}; refused for the session in view or one
   mid-turn); the reply lands beside it as <pid>-<id>.res. sessions are
   addressed by session id only -- never by tab position. a spawn reply is
   held until the backend reports its session id, up to 30 seconds, and then
   carries {"session":"<id>","addr":"<path>"}; if no id appears the reply is an
   error. children see this instance's pid as $MUX_PID, and the path in "addr"
   as $MUX_SESSION_FILE -- the file this session's own id is written to once
   the backend reports one, which is how a child names itself. */
void dispatch_poll(void);

#endif
