#ifndef RELAY_H
#define RELAY_H

struct session;

int  relay_start(struct session *s);
void relay_stop(void);

const char *relay_label(void);
const char *relay_system_note(void);

int relay_fds(int *out, int max);
int relay_pending(void);

struct session *relay_session(void);
void relay_forget_session(struct session *s);

void relay_poll(struct session *live);
void relay_turn_done(struct session *s);

#endif
