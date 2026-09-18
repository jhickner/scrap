#ifndef ORCHEVENT_H
#define ORCHEVENT_H

#include <stddef.h>

/* Completion events, durable on disk.

   A worker's instance is not always the instance running the orchestrator, and
   a worker can finish while no orchestrator is running at all, so an event is a
   file rather than a callback: the worker's instance appends one, and whichever
   instance owns the orchestrator drains it. Nothing is lost across a restart,
   and a delivery that fails is simply not acked and comes round again. Losing
   one entirely still only costs time: reconcile finds the same thing from the
   result files, which stay the real completion signal. */

#define ORCHEVENT_ID_MAX 64

struct orch_event {
    char id[ORCHEVENT_ID_MAX];   /* the event, for acking */
    char task[64];               /* the task the worker was given */
    char session[160];           /* the worker */
    char notify[160];            /* the session that asked to be told */
    char reason[16];             /* "idle" | "exit" */
    long ts;
};

/* Record a completion. A second event for the same task and reason that has not
   been delivered yet is dropped, so a worker that goes idle twice is reported
   once. */
int orchevent_emit(const char *task, const char *session, const char *notify,
                   const char *reason);

/* Undelivered events, oldest first. Caller frees. */
int orchevent_pending(struct orch_event **out);

/* Delivered: drop it. */
int orchevent_ack(const char *id);

#endif
