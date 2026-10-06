
#ifndef TITLE_H
#define TITLE_H

#include <stddef.h>

int title_lookup(const char *id, char *out, size_t size);

int title_set(const char *id, const char *name);

void title_clear(const char *id);

int title_clean(const char *name, char *out, size_t size);

#endif
