#ifndef SESSIONSWITCH_H
#define SESSIONSWITCH_H

#include <stddef.h>

#include "vendor/cJSON.h"

void sessionswitch_run(void);
void sessionswitch_step(int dir);
int  sessionswitch_show_open(const char *id);

int  sessionswitch_gave_last(void);
void sessionswitch_serve_request(void);
int  sessionswitch_yank(const char *target, char *why, size_t size);

cJSON *sessionswitch_rows(void);

#endif
