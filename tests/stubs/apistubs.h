#ifndef APISTUBS_H
#define APISTUBS_H

#include "session.h"
#include "workspace.h"

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
extern int             hide_ids;

void end_turn(struct session *s, const char *reply, int interrupted);

#endif
