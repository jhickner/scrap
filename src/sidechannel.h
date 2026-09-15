
#ifndef SIDECHANNEL_H
#define SIDECHANNEL_H

#include "vendor/cJSON.h"

struct session;

int sidechannel_start(const struct session *s, const char *prompt, const char *label);

/* A forked status update on the running turn; prev is the last one given, so
   the reply covers only what changed since. done gets the answer, or NULL on
   failure, then ud is dropped. */
typedef void (*sidechannel_done)(void *ud, const char *answer);
int  sidechannel_status(const struct session *s, const char *prev,
                        sidechannel_done done, void *ud);
void sidechannel_forget(const struct session *s);

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
