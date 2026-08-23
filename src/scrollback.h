#ifndef SCROLLBACK_H
#define SCROLLBACK_H

#include "vendor/cJSON.h"

int scrollback_restore(const char *path);

static inline const char *scrollback_str(const cJSON *st, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(st, key));
    return s ? s : "";
}

static inline int scrollback_int(const cJSON *st, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(st, key);
    return cJSON_IsNumber(n) ? (int)cJSON_GetNumberValue(n) : 0;
}

#endif
