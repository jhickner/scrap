#ifndef RESTART_H
#define RESTART_H

struct session;

void restart_arm(void);

void restart_shield_thread(void);

void restart_flag(const char *flag);
void restart_unflag(const char *flag);

void restart_request(void);

int restart_wanted(void);

/* Seconds since the pending restart was first seen, or 0 if none is pending. */
double restart_waited(void);

/* How long a pending restart waits for background tasks alone before it stops
 * them (SCRAP_RESTART_BG_WAIT overrides, in seconds). */
double restart_bg_wait(void);

void restart_clear(void);

int restart_exec(struct session *s);

void restart_wake(struct session *s);

#endif
