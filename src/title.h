
#ifndef TITLE_H
#define TITLE_H

#include <stddef.h>

int title_lookup(const char *id, char *out, size_t size);

int title_set(const char *id, const char *name);

/* the name as it would be filed: quotes, spaces and a trailing period off.
   0 when nothing usable is left of it */
int title_clean(const char *name, char *out, size_t size);

void title_request(const char *id, const char *backend, const char *model,
                   const char *cwd, const char *prompt, const char *reply);

#endif
