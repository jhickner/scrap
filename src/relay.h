#ifndef RELAY_H
#define RELAY_H

struct session;

int  relay_start(struct session *s);
void relay_stop(void);

const char *relay_label(void);
const char *relay_system_note(void);

int   relay_fds(int *out, int max);
int   relay_pending(void);
char *relay_take_line(void);
void  relay_run_line(char *line);

struct session *relay_session(void);
void relay_refocus(void);
void relay_forget_session(struct session *s);

int relay_poll(struct session *live);

#endif
