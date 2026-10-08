
#ifndef MD_H
#define MD_H

#include <stddef.h>

#include "ui.h"
#include "vendor/cJSON.h"

void md_render(const char *text, int indent);

char *md_command(const char *url);
char *md_command_at(int row, int col);
char *md_command_nth(const char *text, int nth);

void md_render_kept(const char *text, int indent);

#define MD_KEPT_KIND "md"
void md_kept_load(const cJSON *st);

/* A kept item's text, and hiding a byte span of it: shown again when
 * to <= from; keep makes the cut part of what the scrollback saves. */
const char *md_kept_text(unsigned mark);
void        md_kept_hide(unsigned mark, size_t from, size_t to, int keep);

/* md_render_kept with a byte span hidden from the first draw on; returns the
 * item's mark for md_kept_hide. */
unsigned md_render_kept_hiding(const char *text, int indent, size_t from, size_t to);

#endif
