#ifndef VOICE_H
#define VOICE_H

struct session;

#define VOICE_PREAMBLE \
    "Your reply will be read aloud: be brief, no code or tables unless asked.\n\n"

void voice_init(void);
void voice_stop(void);

int  voice_on(void);
void voice_set_on(int on);

void voice_set_volume(int percent);
int  voice_volume(void);

void voice_set_rate(int percent);
int  voice_rate(void);

void voice_pending(void);

void voice_turn_begin(struct session *s);
void voice_turn_done(struct session *s);
void voice_turn_cancel(struct session *s);

void voice_refocus(void);

void voice_arm(int on);

int  voice_speaking(void);
void voice_mute(void);

char *voice_with_preamble(const char *line);

const char *voice_label(void);

#endif
