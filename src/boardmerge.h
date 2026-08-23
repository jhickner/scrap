
#ifndef BOARDMERGE_H
#define BOARDMERGE_H

#include <stddef.h>

struct board_card;

int boardmerge_base(const struct board_card *c, char *out, size_t size);

int boardmerge_pump(void);

int boardmerge_running(const char *id);

int boardmerge_take(const char *key, const char *out, int ok);

#endif
