#ifndef RELAY_H
#define RELAY_H

struct session;

/* A phone-side chat client over one WebSocket: lines in, replies out. The
   same tabs as the terminal, like --telegram. Settings live in
   ~/.config/mux/relay. */

int  relay_start(struct session *s);
void relay_stop(void);

const char *relay_label(void);

int   relay_fds(int *out, int max);
int   relay_pending(void);
char *relay_take_line(void);
void  relay_run_line(char *line);

struct session *relay_session(void);
void relay_refocus(void);
void relay_forget_session(struct session *s);

/* from the abort check: 1 when the client asked to stop this session's turn.
   Also reports the busy state when it changes. */
int relay_poll(struct session *live);

#endif
