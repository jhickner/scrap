
#ifndef MODELS_H
#define MODELS_H

#include <stddef.h>

struct pick_item;

int models_for(const char *backend, const struct pick_item **out);

int models_codex_slug(const char *model, char *out, size_t size);

struct model_rates {
    double input, output, cache_read, cache_write;
};

int models_rates(const char *backend, const char *model, struct model_rates *out);

const char *models_short_name(const char *backend, const char *model);

#endif
