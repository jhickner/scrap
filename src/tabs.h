#ifndef TABS_H
#define TABS_H

struct session;

/* Sessions a window is bringing up behind the one in front. Each connects off
   the main thread and opens as a tab once its own CLI is up. */

/* Reads a --tabs file: one line per tab, a screen dump path and the argv the
   session was started with. */
void tabs_prepare(const char *path);

/* Queues a session made elsewhere. screen may be NULL for no dump to replay. */
int tabs_queue(struct session *s, const char *screen);

int tabs_pending(void);

/* Connects the front session and everything queued at once. Returns whether
   the front one came up; it is the only one waited for. */
int tabs_start(struct session *front);

/* Opens what has finished connecting. `all` waits for the rest, for a window
   on its way out. */
/* Connects what is queued, for a window already up. */
void tabs_start_queued(void);

void tabs_admit(int all);

void tabs_drop_all(void);

#endif
