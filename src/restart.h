#ifndef RESTART_H
#define RESTART_H

struct session;

void restart_arm(void);

void restart_shield_thread(void);

void restart_flag(const char *flag);
void restart_unflag(const char *flag);

void restart_request(void);

int restart_wanted(void);

void restart_clear(void);

int restart_exec(struct session *s);

#endif
