#ifndef HUD_H
#define HUD_H

#include "vendor/cJSON.h"

struct session;

#define HUD_KIND "hud"

void hud_print(const struct session *s);

void hud_load(const cJSON *st);

/* count this restart on the hud still sitting at the end of the transcript,
   so restarting over and over does not repeat the block. 0 when something
   else has been printed since and the caller has to say so itself. */
int  hud_restarted(void);

#endif
