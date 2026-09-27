#ifndef HUD_H
#define HUD_H

#include "vendor/cJSON.h"

struct session;

#define HUD_KIND "hud"

void hud_print(const struct session *s);
void hud_print_launch(const struct session *s);

void hud_load(const cJSON *st);

int  hud_restarted(void);

#endif
