#ifndef API_H
#define API_H

/* the worker API: HTTP + SSE on the address in ~/.config/mux/api (token,
   bind, port; MUX_API_PORT overrides port). requests are served on
   libmicrohttpd threads and executed on the main thread by api_poll, which
   the idle loop calls when api_fds is readable */

/* 1 when listening; otherwise prints the reason on stderr and returns 0 */
int  api_start(void);
void api_stop(void);
int  api_active(void);

int  api_fds(int *out, int max);
void api_poll(void);

/* turn hooks, called from main.c's turn_begin/turn_done */
struct session;
void api_turn_begin(struct session *s);
void api_turn_done(struct session *s);

#endif
