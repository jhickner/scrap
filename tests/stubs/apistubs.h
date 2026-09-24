#ifndef APISTUBS_H
#define APISTUBS_H

#include "session.h"
#include "workspace.h"

/* stubbed tabs for the worker API tests: a session is a slot with an id, a
   running flag and a queue */
struct session {
    char           id[64];
    int            running;
    int            interrupted;
    char           queue[8][64];
    int            nqueue;
    char          *reply;
    backend_result result;
    char           env[4][64];
    char           model[64];
};

extern struct session *tabs[WORKSPACE_MAX];
extern int             ntabs;
extern int             in_view;
extern int             spawn_fails;
extern int             hide_ids;   /* spawned sessions report no id yet */

/* the turn in s ends; a queued line starts the next one */
void end_turn(struct session *s, const char *reply, int interrupted);

#endif
