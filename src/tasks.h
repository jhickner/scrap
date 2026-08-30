#ifndef TASKS_H
#define TASKS_H

#include <stddef.h>
#include <time.h>

#include "vendor/agents/backend.h"

#define TASKS_MAX 16

/* Work a backend started that outlives the turn that started it: a subagent, a
   detached command. A backend that reports BACKEND_EV_TASK gives the whole life
   cycle; for one that does not, the spawn call is all there is, so those entries
   stay at "launched" -- what ran, not how it ended. */
struct task {
    char   id[40];
    char   desc[140];
    char   type[40];
    char   status[24];
    char   latest[240];
    time_t started, ended;
    int    repeats;
    int    inferred; /* from a spawn tool call, not from a life cycle report */
};

struct tasktab {
    struct task v[TASKS_MAX];
    int         n;
    int         seq;
    int         lifecycle; /* the backend reports BACKEND_EV_TASK */
    char        tag[16];   /* prefix for the ids invented for inferred tasks */
};

void tasks_reset(struct tasktab *t, const char *tag);

/* Feed every event of a turn. Returns the entry whose state changed, or NULL
   when the event says nothing new. *repeat, when asked for, reports a task that
   has already finished reporting again. */
const struct task *tasks_note(struct tasktab *t, const backend_event *ev, int *repeat);

int tasks_done(const struct task *a);
int tasks_running(const struct tasktab *t);

/* Running work the backend has promised to report the end of. An entry inferred
   from a spawn call is not counted: nothing will ever close it. */
int tasks_pending(const struct tasktab *t);
int tasks_count(const struct tasktab *t);

const struct task *tasks_at(const struct tasktab *t, int i);

/* "agent running: review the diff" — one line for a status change. */
/* Close every entry still reading as running: the backend says the work is
   gone, and nothing will report an end for it. Returns how many were closed. */
int tasks_drop(struct tasktab *t);

void tasks_line(const struct task *a, char *out, size_t size);

void tasks_duration(char *out, size_t size, long secs);

#endif
