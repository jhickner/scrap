#ifndef IM_H
#define IM_H

struct session;

int  im_start(struct session *s);
void im_stop(void);

const char *im_label(void);
const char *im_system_note(void);

int   im_fds(int *out, int max);
int   im_pending(void);
char *im_take_line(void);
void  im_run_line(char *line);

struct session *im_session(void);
void im_refocus(void);
void im_forget_session(struct session *s);

#endif
