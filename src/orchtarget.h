#ifndef ORCHTARGET_H
#define ORCHTARGET_H

#include <stddef.h>

#include "orchtask.h"

/* Which mux instance a worker should land in. No registry of its own:
   ~/.config/mux/live already records every session with the pid of the
   instance hosting it, so the instances are whatever is in there and still
   alive. The dispatch protocol is already instance-addressed, since a request
   file is named <pid>-<key>.req. */

struct orch_instance {
    long pid;
    int  sessions;  /* live records for this pid */
    long ts;        /* newest record, as a measure of recent activity */
};

/* Live instances, busiest-recent first. Caller frees. */
int orchtarget_instances(struct orch_instance **out);

/* Room for one more session in this instance. */
int orchtarget_has_room(const struct orch_instance *v);

/* The instance to dispatch into: prefer_pid when it is alive and has room, else
   the most recently active instance with room. Returns 0 when there is none, in
   which case the task stays queued rather than going somewhere arbitrary. */
long orchtarget_pick(long prefer_pid);

/* Fill each task's live field from the session directory, in one pass: what its
   worker is doing right now, or nothing when no live instance has it. */
void orchtarget_apply_live(struct orch_rec *recs, int n);

/* The instance hosting a session id, or 0 when no live instance has it. */
long orchtarget_session_pid(const char *session);

/* Status recorded for a live session ("working", "finished", ...), or NULL when
   the session is not in any live instance. Writes the hosting pid when pid is
   not NULL. */
int orchtarget_session_status(const char *session, char *out, size_t size, long *pid);

#endif
