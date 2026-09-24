#ifndef API_H
#define API_H

/* the worker API: HTTP + SSE on the address in ~/.config/mux/api (token,
   bind, port; MUX_API_PORT overrides port). requests are served on
   libmicrohttpd threads and executed on the main thread by api_poll, which
   the idle loop calls when api_fds is readable */

/* 1 when listening; otherwise prints the reason on stderr and returns 0 */
#include <stddef.h>

int  api_start(void);
void api_stop(void);
int  api_active(void);
const char *api_url(void);
const char *api_token(void);
/* {"name","url","token"}, one field per line, for attaching this host in dlv; caller frees */
char *api_connect_json(void);
/* Adds this API as a host of the dlv in ~/.config/dlv/client.json: 1 registered, -1 failed (msg says why),
   0 when this machine has no dlv client config. */
int  api_register_dlv(char *msg, size_t n);

int  api_fds(int *out, int max);
void api_poll(void);

/* turn hooks, called from main.c's turn_begin/turn_done */
struct session;
void api_turn_begin(struct session *s);
void api_turn_done(struct session *s);

#endif
