
#ifndef MD_H
#define MD_H

#include <stddef.h>

#include "ui.h"
#include "vendor/cJSON.h"

void md_render(const char *text, int indent);

struct md_text;

struct md_text *md_text_parse(const char *text);

void md_text_free(struct md_text *t);

const char *md_text_plain(const struct md_text *t, size_t *len);

void md_text_put(const struct md_text *t, size_t from, size_t len,
                 enum ui_role base);

void md_render_kept(const char *text, int indent);

#define MD_KEPT_KIND "md"
void md_kept_load(const cJSON *st);

#endif
