
#ifndef SIDECHANNEL_H
#define SIDECHANNEL_H

#include "vendor/cJSON.h"

struct session;

int sidechannel_start(const struct session *s, const char *prompt, const char *label);

typedef void (*sidechannel_done)(void *ud, const char *answer);
int  sidechannel_status(const struct session *s, const char *prev,
                        sidechannel_done done, void *ud);
int  sidechannel_inbox(const struct session *s, const char *message,
                       sidechannel_done done, void *ud);
void sidechannel_cancel(sidechannel_done done, void *ud);
void sidechannel_forget(const struct session *s);

void sidechannel_show(const struct session *owner, const char *question,
                      const char *answer, int failed);

int sidechannel_fds(int *out, int max);

void sidechannel_poll(void);

void sidechannel_tick(void);

int  sidechannel_rows(void);
void sidechannel_paint(int budget);

int sidechannel_busy(void);

void sidechannel_close_all(void);

#define SIDECHANNEL_BTW_KIND "btw"
void sidechannel_btw_load(const cJSON *st);

#endif
