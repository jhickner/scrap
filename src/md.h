
#ifndef MD_H
#define MD_H

#include <stddef.h>

#include "ui.h"
#include "vendor/cJSON.h"

void md_render(const char *text, int indent);

void md_render_kept(const char *text, int indent);

#define MD_KEPT_KIND "md"
void md_kept_load(const cJSON *st);

#endif
