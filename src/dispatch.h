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

/* open a background tab, name it, and send it a first line; focus stays on
   the tab in view. every argument but backend may be NULL. returns the new
   tab's index, or -1 if the CLI did not start */
int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *const *env, const char *prompt);

/* send a line to the tab at index, echoing it first when the tab is idle
   (a queued line is echoed when its turn starts). returns workspace_send's result */
int dispatch_send(int at, const char *line);

#endif
