#ifndef BOARDCMD_H
#define BOARDCMD_H

#include <stddef.h>

struct board_card;

int boardcmd_base(const struct board_card *c, char *out, size_t size);

int boardcmd_pump(void);

int boardcmd_running(const char *id);

int boardcmd_take(const char *key, const char *out, int ok);

#define BOARDCMD_KEY "step:"

#endif
