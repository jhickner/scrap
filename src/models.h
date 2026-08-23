
#ifndef MODELS_H
#define MODELS_H

#include <stddef.h>

struct pick_item;

int models_for(const char *backend, const struct pick_item **out);

int models_codex_slug(const char *model, char *out, size_t size);

#endif
