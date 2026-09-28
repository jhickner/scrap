#ifndef HUB_H
#define HUB_H

#include <stddef.h>

#include "vendor/cJSON.h"

#define HUB_HOST_MAX 256

const char *hub_split(const char *target, char *host, size_t size);

int hub_send(const char *host, const char *from, const char *target, const char *text, char *msg,
             size_t size);

char *hub_read(const char *host, const char *target, long turns, long bytes, char *msg,
               size_t size);

cJSON *hub_survey(void);

int hub_main(int argc, char **argv);

#endif
