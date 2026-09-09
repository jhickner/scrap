#ifndef VOICE_H
#define VOICE_H

#include <stddef.h>

struct session;

/* Hands-free voice: the helper app listens, finished turns arrive as prompt
   lines, and the current session's replies are read back. */

int  voice_start(char *err, size_t size);
void voice_stop(void);
int  voice_on(void);

/* what has been heard so far this turn; "" once it is sent or dropped */
void voice_on_heard(void (*fn)(void *ud, const char *text), void *ud);

/* the hud label, NULL when off */
const char *voice_label(void);

int   voice_fds(int *out, int max);
/* drains the helper; 1 when a finished turn is waiting */
int   voice_pending(void);
/* the next finished turn; caller frees */
char *voice_take_line(void);

void voice_turn_begin(struct session *s);
void voice_turn_done(struct session *s);
void voice_turn_cancel(struct session *s);
void voice_refocus(void);
/* stop reading the current reply aloud */
void voice_mute(void);

#endif
