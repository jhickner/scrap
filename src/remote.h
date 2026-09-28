#ifndef REMOTE_H
#define REMOTE_H

#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

Backend *remote_open(const char *target);

const cJSON *remote_history(Backend *b);

const char *remote_prompt(Backend *b);

int remote_connected(Backend *b);

#endif
