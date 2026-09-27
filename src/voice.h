#ifndef VOICE_H
#define VOICE_H

#include <stddef.h>

struct session;

int  voice_start(char *err, size_t size);
void voice_start_poll(void);
int  voice_starting(void);
void voice_stop(void);
void voice_handoff(void);
void voice_protect_handoff(void);

int  voice_restart(char *err, size_t size);
int  voice_on(void);
void voice_set_speak(int on);
int  voice_speak(void);

enum voice_mode {
    VOICE_MODE_AUTO,
    VOICE_MODE_WAKE,
    VOICE_MODE_JEV,
};

int         voice_mode(void);
void        voice_set_mode(int mode);
const char *voice_mode_name(int mode);

int         voice_mode_of(const char *name);
int  voice_wake(void);
void voice_set_wake(int on);

int  voice_apply(int on, int speak, char *err, size_t size);

int  voice_mic(void);
void voice_set_mic(int on);

void voice_mic_off(int every);
void voice_set_volume(int percent);
int  voice_volume(void);

void voice_set_rate(int percent);
int  voice_rate(void);

void   voice_set_silence(double seconds);
double voice_silence(void);

void voice_on_heard(void (*fn)(void *ud, const char *text), void *ud);

void voice_on_draft(const char *(*fn)(void *ud), void *ud);

void voice_on_release(void (*fn)(void *ud), void *ud);

void voice_on_claim(int (*fn)(void *ud, const char *text), void *ud);

const char *voice_label(void);

int   voice_fds(int *out, int max);

int   voice_pending(void);

char *voice_take_line(void);

void voice_turn_begin(struct session *s);
void voice_turn_done(struct session *s);
void voice_turn_cancel(struct session *s);

void voice_refocus(void);

void voice_leave(struct session *s);

void voice_forget(const struct session *s);

void voice_commit(struct session *s);

void voice_draft_sent(void);

int  voice_drop(void);

int  voice_discard(void);

void voice_arm(int on);

char *voice_with_preamble(const char *line, int queued);

int  voice_speaking(void);

void voice_mute(void);

#endif
