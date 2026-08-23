#ifndef TG_H
#define TG_H

struct session;

int tg_start(struct session *s);
void tg_stop(void);

const char *tg_label(void);

int tg_fds(int *out, int max);

char *tg_take_line(void);

int tg_pending(void);

void tg_run_line(char *line);

struct session *tg_session(void);

void tg_refocus(void);

void tg_forget_session(struct session *s);

const char *tg_system_note(void);

#endif
