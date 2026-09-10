#ifndef VOICE_H
#define VOICE_H

#include <stddef.h>

struct session;

/* Hands-free voice: the helper app listens, finished turns arrive as prompt
   lines. Replies are read back unless speak is off. */

int  voice_start(char *err, size_t size);
void voice_stop(void);
/* Ends the shared helper process and reconnects, so a new build takes effect. */
int  voice_restart(char *err, size_t size);
int  voice_on(void);
void voice_set_speak(int on);
int  voice_speak(void);
/* start or stop and remember it so later sessions and restarts match */
int  voice_apply(int on, int speak, char *err, size_t size);
/* the microphone, while replies keep being read aloud */
int  voice_mic(void);
void voice_set_mic(int on);
void voice_set_volume(int percent);
int  voice_volume(void);
/* speaking rate as a percent of normal speed; a reply already being spoken
   keeps the rate it started at */
void voice_set_rate(int percent);
int  voice_rate(void);

/* seconds of quiet that end a spoken turn; longer leaves more room to pause
   mid-sentence. The wait is scaled from this by how finished the turn sounds */
void   voice_set_silence(double seconds);
double voice_silence(void);

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
/* send the in-progress utterance and anything queued to this session, so
   leaving the pane does not drop what was already heard */
void voice_commit(struct session *s);
/* the in-progress utterance was submitted by hand: keep it out of the queue so
   it is not sent a second time when the turn endpoints */
void voice_draft_sent(void);
/* drop the in-progress utterance: clear the preview and do not submit it.
   1 if there was something to drop */
int  voice_drop(void);
/* 0 while this pane is not focused: speech is discarded rather than submitted */
void voice_arm(int on);
/* line with the spoken-reply preamble prefixed, so the answer is shaped for
   the ear; NULL when voice is off or replies are not read aloud. caller frees */
char *voice_with_preamble(const char *line);

/* 1 while a reply is being read aloud */
int  voice_speaking(void);
/* stop reading the current reply aloud */
void voice_mute(void);

#endif
