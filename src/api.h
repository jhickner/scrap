#ifndef API_H
#define API_H

#include <stddef.h>

int  api_start(void);
void api_stop(void);
const char *api_start_error(void);
int  api_active(void);
const char *api_url(void);

char *api_connect_json(void);

int  api_register_dlv(char *msg, size_t n);

int  api_fds(int *out, int max);
void api_poll(void);

struct session;
void api_turn_begin(struct session *s);
void api_turn_done(struct session *s);

#endif
