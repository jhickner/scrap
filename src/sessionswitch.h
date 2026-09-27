#ifndef SESSIONSWITCH_H
#define SESSIONSWITCH_H

void sessionswitch_run(void);
void sessionswitch_step(int dir);
int  sessionswitch_show_open(const char *id);

int  sessionswitch_gave_last(void);
void sessionswitch_serve_request(void);

#endif
